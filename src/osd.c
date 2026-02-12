/*
 * osd.c - Simple on-screen display using OpenGL primitives
 * 
 * Note: This is a basic OSD without font rendering.
 * For a proper OSD, we'd use SDL2_ttf or a bitmap font.
 * For now, we just draw basic status indicators.
 */

#include <stdio.h>
#include <SDL2/SDL_opengl.h>

#include "osd.h"

void osd_init(void) {
    // Nothing to init for basic OSD
}

void osd_cleanup(void) {
    // Nothing to cleanup
}

static void draw_rect(float x, float y, float w, float h, 
                      float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void draw_bar(float x, float y, float w, float h, 
                     float value, float max_value) {
    // Background
    draw_rect(x, y, w, h, 0.2f, 0.2f, 0.2f, 0.8f);
    
    // Fill
    float fill = (value / max_value) * w;
    draw_rect(x, y, fill, h, 0.2f, 0.8f, 0.2f, 0.9f);
    
    // Border
    glColor4f(1.0f, 1.0f, 1.0f, 0.8f);
    glLineWidth(1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void osd_render(const config_t *config, osd_mode_t mode, int width, int height) {
    // Set up 2D orthographic projection
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Draw mode-specific OSD
    float margin = 10.0f;
    float bar_h = 8.0f;
    float bar_w = 100.0f;
    
    switch (mode) {
        case OSD_MODE_STATUS:
            // Show horizontal stretch indicator at bottom
            draw_bar(margin, height - margin - bar_h, bar_w, bar_h,
                    config->h_stretch, 2.0f);
            
            // Show smooth indicator
            if (config->smooth_h) {
                draw_rect(margin + bar_w + 5, height - margin - bar_h, 
                         bar_h, bar_h, 0.2f, 0.6f, 1.0f, 0.9f);
            } else {
                draw_rect(margin + bar_w + 5, height - margin - bar_h,
                         bar_h, bar_h, 1.0f, 0.4f, 0.2f, 0.9f);
            }
            break;
            
        case OSD_MODE_CALIBRATE:
            // Draw crop region indicator
            {
                float cx = margin;
                float cy = margin;
                float cw = 150.0f;
                float ch = 100.0f;
                
                // Background
                draw_rect(cx, cy, cw, ch, 0.0f, 0.0f, 0.0f, 0.7f);
                
                // Crop region visualization
                float scale = 0.1f;
                float crop_x = cx + 5 + config->crop_x * scale;
                float crop_y = cy + 5 + config->crop_y * scale;
                float crop_w = config->crop_w * scale;
                float crop_h = config->crop_h * scale;
                
                // Clamp to box
                if (crop_w > cw - 10) crop_w = cw - 10;
                if (crop_h > ch - 10) crop_h = ch - 10;
                
                draw_rect(crop_x, crop_y, crop_w, crop_h, 
                         0.2f, 0.8f, 0.2f, 0.5f);
            }
            break;
            
        case OSD_MODE_SAVE_PRESET:
            // Show save indicator
            draw_rect(width/2 - 50, height/2 - 15, 100, 30, 
                     0.0f, 0.0f, 0.0f, 0.8f);
            draw_rect(width/2 - 48, height/2 - 13, 96, 26,
                     0.2f, 0.6f, 0.2f, 0.8f);
            break;
            
        case OSD_MODE_LOAD_PRESET:
            // Show load indicator
            draw_rect(width/2 - 50, height/2 - 15, 100, 30,
                     0.0f, 0.0f, 0.0f, 0.8f);
            draw_rect(width/2 - 48, height/2 - 13, 96, 26,
                     0.2f, 0.2f, 0.8f, 0.8f);
            break;
    }
    
    glDisable(GL_BLEND);
    
    // Restore matrices
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}
