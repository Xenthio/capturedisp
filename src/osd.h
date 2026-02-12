/*
 * osd.h - On-screen display
 */

#ifndef OSD_H
#define OSD_H

#include "config.h"

typedef enum {
    OSD_MODE_STATUS,
    OSD_MODE_CALIBRATE,
    OSD_MODE_SAVE_PRESET,
    OSD_MODE_LOAD_PRESET
} osd_mode_t;

void osd_init(void);
void osd_cleanup(void);
void osd_render(const config_t *config, osd_mode_t mode, int width, int height);

#endif
