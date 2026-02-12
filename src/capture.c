/*
 * capture.c - V4L2 video capture with optimized YUYV conversion
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <jpeglib.h>
#include <setjmp.h>

#include "capture.h"

#define BUFFER_COUNT 2  // Lower = less latency, but may drop frames

typedef struct {
    void *start;
    size_t length;
} buffer_t;

static int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

// Optimized YUYV to RGBA - BT.601 full range
static void yuyv_to_rgba_fast(const uint8_t * __restrict__ src, 
                               uint8_t * __restrict__ dst, 
                               int width, int height) {
    const int total = width * height / 2;
    
    for (int i = 0; i < total; i++) {
        int y0 = src[0];
        int u  = src[1];
        int y1 = src[2];
        int v  = src[3];
        src += 4;
        
        int uu = u - 128;
        int vv = v - 128;
        
        int ruv = (359 * vv) >> 8;
        int guv = (88 * uu + 183 * vv) >> 8;
        int buv = (454 * uu) >> 8;
        
        int r0 = y0 + ruv;
        int g0 = y0 - guv;
        int b0 = y0 + buv;
        
        int r1 = y1 + ruv;
        int g1 = y1 - guv;
        int b1 = y1 + buv;
        
        dst[0] = r0 < 0 ? 0 : (r0 > 255 ? 255 : r0);
        dst[1] = g0 < 0 ? 0 : (g0 > 255 ? 255 : g0);
        dst[2] = b0 < 0 ? 0 : (b0 > 255 ? 255 : b0);
        dst[3] = 255;
        
        dst[4] = r1 < 0 ? 0 : (r1 > 255 ? 255 : r1);
        dst[5] = g1 < 0 ? 0 : (g1 > 255 ? 255 : g1);
        dst[6] = b1 < 0 ? 0 : (b1 > 255 ? 255 : b1);
        dst[7] = 255;
        dst += 8;
    }
}

// Error handler for libjpeg
struct jpeg_error_mgr_ext {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    struct jpeg_error_mgr_ext *err = (struct jpeg_error_mgr_ext*)cinfo->err;
    longjmp(err->setjmp_buffer, 1);
}

static void mjpeg_to_rgba(const uint8_t *mjpeg, size_t size, uint8_t *rgba, int width, int height) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr_ext jerr;
    
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        memset(rgba, 0, width * height * 4);
        return;
    }
    
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, mjpeg, size);
    jpeg_read_header(&cinfo, TRUE);
    
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    
    int row_stride = cinfo.output_width * 3;
    uint8_t *row_buffer = malloc(row_stride);
    
    int y = 0;
    while (cinfo.output_scanline < cinfo.output_height && y < height) {
        jpeg_read_scanlines(&cinfo, &row_buffer, 1);
        
        uint8_t *dst = rgba + y * width * 4;
        for (int x = 0; x < width && x < (int)cinfo.output_width; x++) {
            dst[x * 4 + 0] = row_buffer[x * 3 + 0];
            dst[x * 4 + 1] = row_buffer[x * 3 + 1];
            dst[x * 4 + 2] = row_buffer[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
        y++;
    }
    
    free(row_buffer);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
}

capture_ctx_t *capture_open_buffers(const char *device, int width, int height, int num_buffers) {
    capture_ctx_t *ctx = calloc(1, sizeof(capture_ctx_t));
    if (!ctx) return NULL;
    
    strncpy(ctx->device, device, sizeof(ctx->device) - 1);
    
    ctx->fd = open(device, O_RDWR | O_NONBLOCK);
    if (ctx->fd < 0) {
        fprintf(stderr, "Cannot open device %s: %s\n", device, strerror(errno));
        free(ctx);
        return NULL;
    }
    
    struct v4l2_capability cap;
    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "VIDIOC_QUERYCAP failed\n");
        close(ctx->fd);
        free(ctx);
        return NULL;
    }
    
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "Device does not support video capture\n");
        close(ctx->fd);
        free(ctx);
        return NULL;
    }
    
    // Set framerate to 60fps
    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 60;
    xioctl(ctx->fd, VIDIOC_S_PARM, &parm);
    
    // Try MJPEG first (lower bandwidth), then YUYV
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    
    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
            fprintf(stderr, "Failed to set format\n");
            close(ctx->fd);
            free(ctx);
            return NULL;
        }
    }
    
    ctx->width = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;
    ctx->format = fmt.fmt.pix.pixelformat;
    
    printf("Capture: %dx%d %.4s (%d buffers)\n", ctx->width, ctx->height, (char*)&ctx->format, num_buffers);
    
    struct v4l2_requestbuffers req = {0};
    req.count = num_buffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    
    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "VIDIOC_REQBUFS failed\n");
        close(ctx->fd);
        free(ctx);
        return NULL;
    }
    
    ctx->buffer_count = req.count;
    buffer_t *buffers = calloc(req.count, sizeof(buffer_t));
    ctx->buffers = buffers;
    
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QUERYBUF failed\n");
            goto error;
        }
        
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                ctx->fd, buf.m.offset);
        
        if (buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "mmap failed\n");
            goto error;
        }
    }
    
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QBUF failed\n");
            goto error;
        }
    }
    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "VIDIOC_STREAMON failed\n");
        goto error;
    }
    
    ctx->rgb_buffer = malloc(ctx->width * ctx->height * 4);
    
    return ctx;

error:
    for (int i = 0; i < ctx->buffer_count; i++) {
        if (buffers[i].start && buffers[i].start != MAP_FAILED)
            munmap(buffers[i].start, buffers[i].length);
    }
    free(buffers);
    close(ctx->fd);
    free(ctx);
    return NULL;
}

capture_ctx_t *capture_open(const char *device, int width, int height) {
    return capture_open_buffers(device, width, height, BUFFER_COUNT);
}

void capture_close(capture_ctx_t *ctx) {
    if (!ctx) return;
    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    
    buffer_t *buffers = ctx->buffers;
    for (int i = 0; i < ctx->buffer_count; i++) {
        if (buffers[i].start && buffers[i].start != MAP_FAILED) {
            munmap(buffers[i].start, buffers[i].length);
        }
    }
    
    free(buffers);
    free(ctx->rgb_buffer);
    close(ctx->fd);
    free(ctx);
}

// Get raw YUYV pointer for direct texture upload
uint8_t *capture_get_frame_raw(capture_ctx_t *ctx, size_t *out_size) {
    if (!ctx) return NULL;
    
    buffer_t *buffers = ctx->buffers;
    struct v4l2_buffer buf = {0};
    
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return NULL;
        return NULL;
    }
    
    ctx->current_index = buf.index;
    if (out_size) *out_size = buf.bytesused;
    
    return buffers[buf.index].start;
}

void capture_return_buffer(capture_ctx_t *ctx) {
    if (!ctx) return;
    
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = ctx->current_index;
    
    xioctl(ctx->fd, VIDIOC_QBUF, &buf);
}

// Get converted RGBA frame
uint8_t *capture_get_frame(capture_ctx_t *ctx) {
    if (!ctx) return NULL;
    
    size_t size;
    uint8_t *raw = capture_get_frame_raw(ctx, &size);
    if (!raw) return NULL;
    
    if (ctx->format == V4L2_PIX_FMT_YUYV) {
        yuyv_to_rgba_fast(raw, ctx->rgb_buffer, ctx->width, ctx->height);
    } else if (ctx->format == V4L2_PIX_FMT_MJPEG) {
        mjpeg_to_rgba(raw, size, ctx->rgb_buffer, ctx->width, ctx->height);
    }
    
    capture_return_buffer(ctx);
    
    return ctx->rgb_buffer;
}
