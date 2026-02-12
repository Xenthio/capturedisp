/*
 * capturedisp - Fast NES capture display for CRT TVs
 * 
 * Optimized for low latency: crop first, then scale the smaller region
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "capture.h"
#include "config.h"

#define WINDOW_TITLE "capturedisp"

// NES Switch Online 1080p capture parameters (built-in preset)
#define NES_CROP_X 448
#define NES_CROP_Y 83
#define NES_CROP_W 1024
#define NES_CROP_H 912
#define NES_NATIVE_W 256
#define NES_NATIVE_H 228

// Current crop settings (can be changed by presets)
static int crop_x = NES_CROP_X;
static int crop_y = NES_CROP_Y;
static int crop_w = NES_CROP_W;
static int crop_h = NES_CROP_H;

typedef enum {
    SCALE_SMOOTH,   // 4:3 bilinear horizontal only
    SCALE_PIXEL     // Pixel-perfect nearest
} scale_mode_t;

typedef enum {
    COLOR_PAL60,
    COLOR_NTSC
} color_mode_t;

typedef enum {
    UI_NORMAL,
    UI_SAVE_PRESET,
    UI_LOAD_PRESET
} ui_mode_t;

typedef enum {
    PRESET_NONE,        // Full frame (Switch menu, etc)
    PRESET_NES_SWITCH,
    PRESET_SNES_SWITCH
} detected_preset_t;

static volatile bool running = true;
static config_t config;
static bool show_osd = true;
static TTF_Font *font = NULL;
static scale_mode_t scale_mode = SCALE_SMOOTH;
static color_mode_t color_mode = COLOR_PAL60;
static bool current_240p_mode = false;
static capture_ctx_t *capture = NULL;
static ui_mode_t ui_mode = UI_NORMAL;
static bool auto_detect = false;
static detected_preset_t last_detected = PRESET_NONE;
static int detect_cooldown = 0;  // Frames until next detection
static int last_border_luma[4] = {0};  // Track border brightness to detect actual changes
static bool pending_border_scan = false;  // D key pressed, scan on next frame
static int buffer_count = 2;  // V4L2 buffer count (1-4, lower = less latency)
static bool pending_buffer_change = false;

// Preset menu state
static char **preset_names = NULL;
static int preset_count = 0;
static int preset_selected = 0;
static char preset_input[32] = "";
static int preset_input_len = 0;

void set_color_mode(color_mode_t mode) {
    if (mode == COLOR_PAL60) {
        printf("Applying PAL60 color...\n");
        system("sudo python3 ~/tweakvec/tweakvec.py --preset PAL60 2>/dev/null");
    } else {
        printf("Applying NTSC color...\n");
        system("sudo python3 ~/tweakvec/tweakvec.py --preset NTSC 2>/dev/null");
    }
    color_mode = mode;
}

void set_video_mode(bool use_240p) {
    if (use_240p) {
        printf("Switching to 240p...\n");
        system("tvservice -c 'NTSC 4:3 P' 2>/dev/null");
    } else {
        printf("Switching to 480i...\n");
        system("tvservice -c 'NTSC 4:3' 2>/dev/null");
    }
    
    usleep(100000);
    set_color_mode(color_mode);  // Re-apply color mode after tvservice
    current_240p_mode = use_240p;
}

void signal_handler(int sig) {
    (void)sig;
    running = false;
}

// Sample a YUYV pixel and return Y (luma) value
static inline int sample_yuyv_luma(const uint8_t *yuyv, int width, int x, int y) {
    // YUYV: Y0 U Y1 V - each pixel pair is 4 bytes
    return yuyv[(y * width + x) * 2];
}

// Sample RGB from YUYV at a point
static void sample_yuyv_rgb(const uint8_t *yuyv, int width, int x, int y, int *r, int *g, int *b) {
    int idx = (y * width + (x & ~1)) * 2;
    int y_val = yuyv[(y * width + x) * 2];
    int u = yuyv[idx + 1] - 128;
    int v = yuyv[idx + 3] - 128;
    
    *r = y_val + ((359 * v) >> 8);
    *g = y_val - ((88 * u + 183 * v) >> 8);
    *b = y_val + ((454 * u) >> 8);
    
    if (*r < 0) *r = 0; if (*r > 255) *r = 255;
    if (*g < 0) *g = 0; if (*g > 255) *g = 255;
    if (*b < 0) *b = 0; if (*b > 255) *b = 255;
}

// Auto-detect which preset to use based on border analysis
// Returns true if border has changed significantly (worth re-evaluating)
static bool border_changed(const uint8_t *yuyv, int width, detected_preset_t current) {
    int samples[4];
    samples[0] = sample_yuyv_luma(yuyv, width, 400, 200);
    samples[1] = sample_yuyv_luma(yuyv, width, 400, 400);
    samples[2] = sample_yuyv_luma(yuyv, width, 400, 600);
    samples[3] = sample_yuyv_luma(yuyv, width, 400, 800);
    
    int diff = 0;
    for (int i = 0; i < 4; i++) {
        int d = samples[i] - last_border_luma[i];
        if (d < 0) d = -d;
        diff += d;
        last_border_luma[i] = samples[i];
    }
    
    // If currently NONE (no crop), always re-check to detect game start
    if (current == PRESET_NONE) {
        return true;
    }
    
    // Only consider it changed if total difference > threshold
    return diff > 60;  // ~15 per sample average
}

// Scan frame to detect game area borders automatically
// Returns true if a bordered game area was found
static bool scan_for_game_area(const uint8_t *yuyv, int width, int height,
                                int *out_x, int *out_y, int *out_w, int *out_h) {
    // The "black" border might be dithered dark gray (~luma 20-25)
    // Content threshold needs to be higher
    const int content_threshold = 40;
    const int border_threshold = 30;  // Below this is considered border
    
    // First, sample the border area to get baseline darkness
    int border_luma = sample_yuyv_luma(yuyv, width, 200, height / 2);
    
    // Scan from left to find where content starts (skip first 150px for P1 icon)
    int left_edge = 0;
    for (int x = 150; x < width / 2; x += 2) {
        int luma = sample_yuyv_luma(yuyv, width, x, height / 2);
        if (luma > content_threshold && luma > border_luma + 15) {
            left_edge = x;
            break;
        }
    }
    
    // Scan from right
    int right_edge = width;
    for (int x = width - 150; x > width / 2; x -= 2) {
        int luma = sample_yuyv_luma(yuyv, width, x, height / 2);
        if (luma > content_threshold && luma > border_luma + 15) {
            right_edge = x + 1;
            break;
        }
    }
    
    // Scan from top (skip first 120px for overlay icons)
    int center_x = (left_edge + right_edge) / 2;
    if (center_x < 200) center_x = width / 2;
    
    int top_edge = 0;
    for (int y = 120; y < height / 2; y += 2) {
        int luma = sample_yuyv_luma(yuyv, width, center_x, y);
        if (luma > content_threshold && luma > border_luma + 15) {
            top_edge = y;
            break;
        }
    }
    
    // Scan from bottom (use left side of game area to avoid Switch overlay on right)
    int scan_x_bottom = left_edge > 0 ? left_edge + 50 : width / 3;
    int bottom_edge = height;
    for (int y = height - 100; y > height / 2; y -= 2) {
        int luma = sample_yuyv_luma(yuyv, width, scan_x_bottom, y);
        if (luma > content_threshold && luma > border_luma + 15) {
            bottom_edge = y + 1;
            break;
        }
    }
    
    // Validate - need reasonable borders
    int detected_w = right_edge - left_edge;
    int detected_h = bottom_edge - top_edge;
    
    printf("Scan result: left=%d top=%d right=%d bottom=%d (border_luma=%d)\n", 
           left_edge, top_edge, right_edge, bottom_edge, border_luma);
    
    if (left_edge < 50 || detected_w < 200 || detected_h < 200) {
        return false;  // No clear border found
    }
    
    // Snap to 4-pixel boundaries (for clean scaling)
    left_edge = (left_edge) & ~3;
    top_edge = (top_edge) & ~3;
    detected_w = ((right_edge - left_edge) + 3) & ~3;
    detected_h = ((bottom_edge - top_edge) + 3) & ~3;
    
    *out_x = left_edge;
    *out_y = top_edge;
    *out_w = detected_w;
    *out_h = detected_h;
    
    return true;
}

static detected_preset_t detect_preset(const uint8_t *yuyv, int width, int height) {
    (void)height;
    
    // Check if we have black border at x=400 (inside margin, outside game)
    // If this area is NOT black, we're probably on Switch menu
    int border_y1 = sample_yuyv_luma(yuyv, width, 400, 300);
    int border_y2 = sample_yuyv_luma(yuyv, width, 400, 500);
    int border_y3 = sample_yuyv_luma(yuyv, width, 400, 700);
    
    // If border area is not dark, probably Switch menu - no crop
    if (border_y1 > 30 || border_y2 > 30 || border_y3 > 30) {
        return PRESET_NONE;
    }
    
    // Border is black - we're in a game. Now detect NES vs SNES.
    // Check y=85 at center - NES has game content here, SNES still has border
    int y85_luma = sample_yuyv_luma(yuyv, width, 700, 85);
    int y95_luma = sample_yuyv_luma(yuyv, width, 700, 95);
    
    // NES game area starts at y=83, so y=85 should have content (non-black)
    // SNES game area starts at y=92, so y=85 is still black border
    
    if (y85_luma > 20) {
        // Content at y=85 = NES (game starts at y=83)
        return PRESET_NES_SWITCH;
    } else if (y95_luma > 20) {
        // No content at y=85 but content at y=95 = SNES (game starts at y=92)
        return PRESET_SNES_SWITCH;
    }
    
    // Black screen in game area - could be loading, default to NES
    // Or check more samples to be sure
    int center_luma = sample_yuyv_luma(yuyv, width, 960, 540);
    if (center_luma > 10) {
        // There's something in center, check the game height
        // Scan down from y=83 to find content
        int nes_start = sample_yuyv_luma(yuyv, width, 700, 83);
        if (nes_start > 15) {
            return PRESET_NES_SWITCH;
        }
        return PRESET_SNES_SWITCH;
    }
    
    // Very dark/black game screen - keep previous or default
    return PRESET_NONE;
}

static void apply_detected_preset(detected_preset_t preset, 
                                   int *cx, int *cy, int *cw, int *ch) {
    switch (preset) {
        case PRESET_NES_SWITCH:
            *cx = 448; *cy = 83; *cw = 1024; *ch = 912;
            break;
        case PRESET_SNES_SWITCH:
            *cx = 448; *cy = 92; *cw = 1024; *ch = 896;
            break;
        case PRESET_NONE:
        default:
            *cx = 0; *cy = 0; *cw = 1920; *ch = 1080;
            break;
    }
}

// Fast YUYV crop + convert - only process the cropped region
static void yuyv_crop_to_rgba(const uint8_t *src, int src_w, int src_h,
                               uint8_t *dst, 
                               int crop_x, int crop_y, int crop_w, int crop_h) {
    (void)src_h;
    
    // Make sure crop_x is even (YUYV has 2-pixel macropixels)
    crop_x &= ~1;
    
    for (int y = 0; y < crop_h; y++) {
        const uint8_t *row = src + ((crop_y + y) * src_w + crop_x) * 2;
        uint8_t *out = dst + y * crop_w * 4;
        
        for (int x = 0; x < crop_w; x += 2) {
            int y0 = row[0];
            int u  = row[1];
            int y1 = row[2];
            int v  = row[3];
            row += 4;
            
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
            
            out[0] = r0 < 0 ? 0 : (r0 > 255 ? 255 : r0);
            out[1] = g0 < 0 ? 0 : (g0 > 255 ? 255 : g0);
            out[2] = b0 < 0 ? 0 : (b0 > 255 ? 255 : b0);
            out[3] = 255;
            out[4] = r1 < 0 ? 0 : (r1 > 255 ? 255 : r1);
            out[5] = g1 < 0 ? 0 : (g1 > 255 ? 255 : g1);
            out[6] = b1 < 0 ? 0 : (b1 > 255 ? 255 : b1);
            out[7] = 255;
            out += 8;
        }
    }
}

void draw_text(SDL_Renderer *renderer, int x, int y, const char *text, SDL_Color color) {
    if (!font || !text || !text[0]) return;
    
    SDL_Surface *surface = TTF_RenderText_Blended(font, text, color);
    if (!surface) return;
    
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
    if (tex) {
        SDL_Rect dst = {x, y, surface->w, surface->h};
        SDL_RenderCopy(renderer, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surface);
}

void free_preset_list(void) {
    if (preset_names) {
        for (int i = 0; i < preset_count; i++) {
            free(preset_names[i]);
        }
        free(preset_names);
        preset_names = NULL;
        preset_count = 0;
    }
}

void load_preset_list(void) {
    free_preset_list();
    preset_count = config_list_presets(&preset_names);
    preset_selected = 0;
}

void draw_osd(SDL_Renderer *renderer, int width, int height) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Color white = {255, 255, 255, 255};
    SDL_Color green = {100, 255, 100, 255};
    SDL_Color yellow = {255, 255, 100, 255};
    
    // Save preset dialog
    if (ui_mode == UI_SAVE_PRESET) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 230);
        SDL_Rect dialog = {width/2 - 160, height/2 - 50, 320, 100};
        SDL_RenderFillRect(renderer, &dialog);
        SDL_SetRenderDrawColor(renderer, 100, 150, 255, 255);
        SDL_RenderDrawRect(renderer, &dialog);
        
        draw_text(renderer, width/2 - 140, height/2 - 40, "Save preset - type name:", white);
        
        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_Rect input_box = {width/2 - 140, height/2 - 10, 280, 30};
        SDL_RenderFillRect(renderer, &input_box);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &input_box);
        
        char display[36];
        snprintf(display, sizeof(display), "%s_", preset_input);
        draw_text(renderer, width/2 - 135, height/2 - 5, display, green);
        draw_text(renderer, width/2 - 140, height/2 + 25, "Enter=Save  Esc=Cancel", white);
        return;
    }
    
    // Load preset dialog
    if (ui_mode == UI_LOAD_PRESET) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 230);
        int menu_h = 80 + (preset_count > 0 ? preset_count : 1) * 20;
        if (menu_h > height - 40) menu_h = height - 40;
        SDL_Rect dialog = {width/2 - 160, 20, 320, menu_h};
        SDL_RenderFillRect(renderer, &dialog);
        SDL_SetRenderDrawColor(renderer, 100, 150, 255, 255);
        SDL_RenderDrawRect(renderer, &dialog);
        
        draw_text(renderer, width/2 - 150, 25, "Load preset:", white);
        
        // Built-in presets first
        draw_text(renderer, width/2 - 150, 50, "[Built-in]", yellow);
        const char *builtins[] = {"NES-Switch-1080p", "SNES-Switch-1080p", NULL};
        int y_pos = 70;
        int bi = 0;
        for (int i = 0; builtins[i]; i++) {
            SDL_Color c = (preset_selected == i) ? green : white;
            char line[64];
            snprintf(line, sizeof(line), "%s %s", (preset_selected == i) ? ">" : " ", builtins[i]);
            draw_text(renderer, width/2 - 140, y_pos, line, c);
            y_pos += 18;
            bi++;
        }
        
        // User presets
        if (preset_count > 0) {
            draw_text(renderer, width/2 - 150, y_pos + 5, "[User]", yellow);
            y_pos += 25;
            for (int i = 0; i < preset_count && y_pos < menu_h; i++) {
                SDL_Color c = (preset_selected == bi + i) ? green : white;
                char line[64];
                snprintf(line, sizeof(line), "%s %s", (preset_selected == bi + i) ? ">" : " ", preset_names[i]);
                draw_text(renderer, width/2 - 140, y_pos, line, c);
                y_pos += 18;
            }
        }
        
        draw_text(renderer, width/2 - 150, menu_h - 5, "Up/Down Enter=Load Esc=Cancel", white);
        return;
    }
    
    // Normal OSD bar
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    SDL_Rect bar = {0, height - 22, width, 22};
    SDL_RenderFillRect(renderer, &bar);
    
    static Uint32 last_fps_time = 0;
    static int frame_count = 0;
    static float current_fps = 0;
    frame_count++;
    Uint32 now = SDL_GetTicks();
    if (now - last_fps_time >= 1000) {
        current_fps = frame_count * 1000.0f / (now - last_fps_time);
        frame_count = 0;
        last_fps_time = now;
    }
    
    char info[128];
    const char *auto_str = auto_detect ? "AUTO" : "Manual";
    const char *preset_str = "";
    if (auto_detect) {
        switch (last_detected) {
            case PRESET_NES_SWITCH: preset_str = "[NES]"; break;
            case PRESET_SNES_SWITCH: preset_str = "[SNES]"; break;
            default: preset_str = "[None]"; break;
        }
    }
    snprintf(info, sizeof(info), "%.1ffps %s%s %s %s %s B%d | A=Auto S V C B", 
             current_fps,
             auto_str, preset_str,
             scale_mode == SCALE_PIXEL ? "Px" : "Sm",
             config.use_240p ? "240p" : "480i",
             color_mode == COLOR_PAL60 ? "PAL60" : "NTSC",
             buffer_count);
    draw_text(renderer, 10, height - 18, info, white);
}

int main(int argc, char *argv[]) {
    const char *device = "/dev/video0";
    bool fullscreen = true;
    
    static struct option long_opts[] = {
        {"device", required_argument, 0, 'd'},
        {"pixel", no_argument, 0, 'x'},
        {"windowed", no_argument, 0, 'w'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "d:xwh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'd': device = optarg; break;
            case 'x': scale_mode = SCALE_PIXEL; break;
            case 'w': fullscreen = false; break;
            case 'h': 
            default:
                printf("Usage: %s [options]\n", argv[0]);
                printf("  -d, --device PATH   Capture device\n");
                printf("  -x, --pixel         Pixel-perfect mode\n");
                printf("  -w, --windowed      Windowed mode\n");
                printf("\nControls: S=Scale, V=Video, O=OSD, F=Fullscreen, Q=Quit\n");
                return opt == 'h' ? 0 : 1;
        }
    }
    
    config_init(&config);
    config_load(&config);
    set_video_mode(config.use_240p);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }
    
    const char *font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    for (int i = 0; font_paths[i]; i++) {
        font = TTF_OpenFont(font_paths[i], 14);
        if (font) break;
    }
    
    Uint32 window_flags = SDL_WINDOW_SHOWN;
    if (fullscreen) window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    
    SDL_Window *window = SDL_CreateWindow(WINDOW_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        720, 480, window_flags);
    
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        TTF_Quit(); SDL_Quit();
        return 1;
    }
    
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    
    // Open capture
    capture = capture_open(device, 1920, 1080);
    if (!capture) {
        fprintf(stderr, "Failed to open %s\n", device);
        SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
        TTF_Quit(); SDL_Quit();
        return 1;
    }
    
    printf("Capture: %dx%d, Crop: %dx%d\n", capture->width, capture->height, crop_w, crop_h);
    
    // Create texture for cropped region only (much smaller!)
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_mode == SCALE_PIXEL ? "0" : "1");
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING, crop_w, crop_h);
    
    // Buffer for cropped RGBA
    uint8_t *crop_buffer = malloc(crop_w * crop_h * 4);
    
    if (fullscreen) SDL_ShowCursor(SDL_DISABLE);
    
    printf("Controls: S=Scale, V=Video, C=Color, O=OSD, F1=Save, F2=Load, Q=Quit\n");
    
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            
            // Text input for save preset dialog
            if (event.type == SDL_TEXTINPUT && ui_mode == UI_SAVE_PRESET) {
                if (preset_input_len < 28) {
                    char c = event.text.text[0];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
                        (c >= '0' && c <= '9') || c == '_' || c == '-') {
                        preset_input[preset_input_len++] = c;
                        preset_input[preset_input_len] = '\0';
                    }
                }
                continue;
            }
            
            if (event.type == SDL_KEYDOWN) {
                SDL_Keycode key = event.key.keysym.sym;
                
                // Save preset mode
                if (ui_mode == UI_SAVE_PRESET) {
                    switch (key) {
                        case SDLK_ESCAPE:
                            ui_mode = UI_NORMAL;
                            preset_input[0] = '\0';
                            preset_input_len = 0;
                            SDL_StopTextInput();
                            break;
                        case SDLK_RETURN:
                            if (preset_input_len > 0) {
                                config.crop_x = crop_x;
                                config.crop_y = crop_y;
                                config.crop_w = crop_w;
                                config.crop_h = crop_h;
                                config_save_preset(&config, preset_input);
                                printf("Saved preset: %s\n", preset_input);
                            }
                            ui_mode = UI_NORMAL;
                            preset_input[0] = '\0';
                            preset_input_len = 0;
                            SDL_StopTextInput();
                            break;
                        case SDLK_BACKSPACE:
                            if (preset_input_len > 0) preset_input[--preset_input_len] = '\0';
                            break;
                    }
                    continue;
                }
                
                // Load preset mode
                if (ui_mode == UI_LOAD_PRESET) {
                    int total = 2 + preset_count;  // 2 built-in + user presets
                    switch (key) {
                        case SDLK_ESCAPE:
                            ui_mode = UI_NORMAL;
                            free_preset_list();
                            break;
                        case SDLK_UP:
                            if (preset_selected > 0) preset_selected--;
                            break;
                        case SDLK_DOWN:
                            if (preset_selected < total - 1) preset_selected++;
                            break;
                        case SDLK_RETURN:
                            {
                                const char *name = NULL;
                                if (preset_selected == 0) {
                                    name = "NES-Switch-1080p";
                                } else if (preset_selected == 1) {
                                    name = "SNES-Switch-1080p";
                                } else if (preset_names && preset_selected - 2 < preset_count) {
                                    name = preset_names[preset_selected - 2];
                                }
                                if (name && config_load_preset(&config, name)) {
                                    crop_x = config.crop_x;
                                    crop_y = config.crop_y;
                                    crop_w = config.crop_w;
                                    crop_h = config.crop_h;
                                    // Recreate texture for new size
                                    SDL_DestroyTexture(texture);
                                    free(crop_buffer);
                                    crop_buffer = malloc(crop_w * crop_h * 4);
                                    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_mode == SCALE_PIXEL ? "0" : "1");
                                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STREAMING, crop_w, crop_h);
                                    printf("Loaded preset: %s (%dx%d at %d,%d)\n", name, crop_w, crop_h, crop_x, crop_y);
                                }
                            }
                            ui_mode = UI_NORMAL;
                            free_preset_list();
                            break;
                    }
                    continue;
                }
                
                // Normal mode
                switch (key) {
                    case SDLK_ESCAPE:
                    case SDLK_q:
                        running = false;
                        break;
                        
                    case SDLK_s:
                        scale_mode = (scale_mode == SCALE_SMOOTH) ? SCALE_PIXEL : SCALE_SMOOTH;
                        SDL_DestroyTexture(texture);
                        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_mode == SCALE_PIXEL ? "0" : "1");
                        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                            SDL_TEXTUREACCESS_STREAMING, crop_w, crop_h);
                        printf("Scale: %s\n", scale_mode == SCALE_PIXEL ? "pixel" : "smooth");
                        break;
                        
                    case SDLK_v:
                        config.use_240p = !config.use_240p;
                        set_video_mode(config.use_240p);
                        break;
                        
                    case SDLK_c:
                        color_mode = (color_mode == COLOR_PAL60) ? COLOR_NTSC : COLOR_PAL60;
                        set_color_mode(color_mode);
                        break;
                        
                    case SDLK_a:
                        auto_detect = !auto_detect;
                        printf("Auto-detect: %s\n", auto_detect ? "ON" : "OFF");
                        if (!auto_detect) {
                            // When turning off, keep current crop
                        }
                        break;
                        
                    case SDLK_d:
                        // Detect border and apply as current crop
                        pending_border_scan = true;
                        printf("Scanning for game border...\n");
                        break;
                    
                    case SDLK_b:
                        // Cycle buffer count 1-4
                        buffer_count++;
                        if (buffer_count > 4) buffer_count = 1;
                        pending_buffer_change = true;
                        printf("Buffer count: %d (will reinit capture)\n", buffer_count);
                        break;
                        
                    case SDLK_o:
                        show_osd = !show_osd;
                        break;
                        
                    case SDLK_f:
                        {
                            Uint32 flags = SDL_GetWindowFlags(window);
                            if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                                SDL_SetWindowFullscreen(window, 0);
                                SDL_ShowCursor(SDL_ENABLE);
                            } else {
                                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                                SDL_ShowCursor(SDL_DISABLE);
                            }
                        }
                        break;
                        
                    case SDLK_F1:
                        ui_mode = UI_SAVE_PRESET;
                        preset_input[0] = '\0';
                        preset_input_len = 0;
                        SDL_StartTextInput();
                        break;
                        
                    case SDLK_F2:
                        load_preset_list();
                        ui_mode = UI_LOAD_PRESET;
                        break;
                }
            }
        }
        
        // Reinit capture if buffer count changed
        if (pending_buffer_change) {
            pending_buffer_change = false;
            capture_close(capture);
            capture = capture_open_buffers(device, 1920, 1080, buffer_count);
            if (!capture) {
                fprintf(stderr, "Failed to reinit capture with %d buffers\n", buffer_count);
                running = false;
                continue;
            }
            printf("Capture reinit: %d buffers\n", capture->buffer_count);
        }
        
        // Get raw YUYV frame
        size_t raw_size;
        uint8_t *raw = capture_get_frame_raw(capture, &raw_size);
        if (raw) {
            // Manual border scan (D key)
            if (pending_border_scan) {
                pending_border_scan = false;
                int new_cx, new_cy, new_cw, new_ch;
                if (scan_for_game_area(raw, capture->width, capture->height,
                                       &new_cx, &new_cy, &new_cw, &new_ch)) {
                    printf("Detected game area: %dx%d at (%d,%d)\n", new_cw, new_ch, new_cx, new_cy);
                    printf("Native resolution: %dx%d\n", new_cw / 4, new_ch / 4);
                    
                    // Apply the detected crop
                    if (new_cw != crop_w || new_ch != crop_h) {
                        crop_x = new_cx; crop_y = new_cy;
                        crop_w = new_cw; crop_h = new_ch;
                        
                        SDL_DestroyTexture(texture);
                        free(crop_buffer);
                        crop_buffer = malloc(crop_w * crop_h * 4);
                        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_mode == SCALE_PIXEL ? "0" : "1");
                        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                            SDL_TEXTUREACCESS_STREAMING, crop_w, crop_h);
                    } else {
                        crop_x = new_cx; crop_y = new_cy;
                    }
                    
                    // Update config for saving
                    config.crop_x = crop_x;
                    config.crop_y = crop_y;
                    config.crop_w = crop_w;
                    config.crop_h = crop_h;
                    
                    // Disable auto-detect when manually scanning
                    auto_detect = false;
                    last_detected = PRESET_NONE;
                    
                    printf("Press F1 to save as preset\n");
                } else {
                    printf("No game border detected\n");
                }
            }
            
            // Auto-detect preset if enabled (check every 30 frames ~1 sec)
            // Only re-evaluate if the border area has actually changed
            if (auto_detect && detect_cooldown <= 0) {
                if (border_changed(raw, capture->width, last_detected)) {
                    detected_preset_t detected = detect_preset(raw, capture->width, capture->height);
                    if (detected != last_detected) {
                        int new_cx, new_cy, new_cw, new_ch;
                        apply_detected_preset(detected, &new_cx, &new_cy, &new_cw, &new_ch);
                        
                        // Only reallocate if size changed
                        if (new_cw != crop_w || new_ch != crop_h) {
                            crop_x = new_cx; crop_y = new_cy;
                            crop_w = new_cw; crop_h = new_ch;
                            
                            SDL_DestroyTexture(texture);
                            free(crop_buffer);
                            crop_buffer = malloc(crop_w * crop_h * 4);
                            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_mode == SCALE_PIXEL ? "0" : "1");
                            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STREAMING, crop_w, crop_h);
                            
                            const char *names[] = {"None", "NES", "SNES"};
                            printf("Auto-detected: %s (%dx%d)\n", names[detected], crop_w, crop_h);
                        } else {
                            crop_x = new_cx; crop_y = new_cy;
                        }
                        last_detected = detected;
                    }
                }
                detect_cooldown = 30;
            }
            if (detect_cooldown > 0) detect_cooldown--;
            
            // Convert only the cropped region
            yuyv_crop_to_rgba(raw, capture->width, capture->height,
                              crop_buffer, crop_x, crop_y, crop_w, crop_h);
            capture_return_buffer(capture);
            
            SDL_UpdateTexture(texture, NULL, crop_buffer, crop_w * 4);
        }
        
        // Render
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        
        int out_w, out_h;
        SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
        
        // Debug first frame
        static bool first = true;
        if (first) {
            printf("Output size: %dx%d\n", out_w, out_h);
            first = false;
        }
        
        // Calculate output size - integer vertical scaling for scanline alignment
        // Native size = crop size / 4 (since capture is 4x scaled)
        int native_w = crop_w / 4;
        int native_h = crop_h / 4;
        
        int dst_w, dst_h;
        if (scale_mode == SCALE_PIXEL) {
            // Pixel-perfect: native * 2
            dst_w = native_w * 2;
            dst_h = native_h * 2;
        } else {
            // Smooth: integer vertical scale, 4:3 horizontal unless wider
            dst_h = native_h * 2;
            int aspect_w = (dst_h * 4) / 3;
            int native_scaled_w = native_w * 2;
            // Use whichever is wider - don't squash wide content
            dst_w = (native_scaled_w > aspect_w) ? native_scaled_w : aspect_w;
        }
        
        int dst_x = (out_w - dst_w) / 2;
        int dst_y = (out_h - dst_h) / 2;
        
        SDL_Rect dst = {dst_x, dst_y, dst_w, dst_h};
        SDL_RenderCopy(renderer, texture, NULL, &dst);
        
        if (show_osd) draw_osd(renderer, out_w, out_h);
        
        SDL_RenderPresent(renderer);
    }
    
    // Cleanup
    free(crop_buffer);
    capture_close(capture);
    SDL_DestroyTexture(texture);
    if (font) TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    
    config_save(&config);
    
    return 0;
}
