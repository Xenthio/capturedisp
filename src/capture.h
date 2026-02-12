/*
 * capture.h - V4L2 video capture
 */

#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int fd;
    int width;
    int height;
    uint32_t format;
    
    void *buffers;
    int buffer_count;
    int current_index;
    
    uint8_t *rgb_buffer;
    
    char device[256];  // Store device path for reinit
} capture_ctx_t;

capture_ctx_t *capture_open(const char *device, int width, int height);
capture_ctx_t *capture_open_buffers(const char *device, int width, int height, int num_buffers);
void capture_close(capture_ctx_t *ctx);
uint8_t *capture_get_frame(capture_ctx_t *ctx);
uint8_t *capture_get_frame_raw(capture_ctx_t *ctx, size_t *out_size);
void capture_return_buffer(capture_ctx_t *ctx);

#endif
