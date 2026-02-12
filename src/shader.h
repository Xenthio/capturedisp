/*
 * shader.h - OpenGL shader for scanline-snapped rendering
 */

#ifndef SHADER_H
#define SHADER_H

#include <stdbool.h>
#include <SDL2/SDL_opengl.h>

#include "config.h"

bool shader_init(void);
void shader_cleanup(void);
void shader_render(GLuint texture, const config_t *config, 
                   int src_width, int src_height,
                   int dst_width, int dst_height);

#endif
