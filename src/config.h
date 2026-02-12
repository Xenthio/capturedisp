/*
 * config.h - Configuration and preset management
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

typedef struct {
    // Crop settings (in source pixels)
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    
    // Display settings
    float h_stretch;    // Horizontal stretch factor (1.0 = 1:1)
    bool smooth_h;      // Smooth horizontal interpolation
    bool use_240p;      // Use 240p output (vs 480i)
    
    // Calibration
    int scanline_offset;  // Vertical offset for scanline alignment
} config_t;

void config_init(config_t *config);
bool config_load(config_t *config);
bool config_save(const config_t *config);
bool config_load_preset(config_t *config, const char *name);
bool config_save_preset(const config_t *config, const char *name);
int config_list_presets(char ***names);

#endif
