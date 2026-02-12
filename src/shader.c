/*
 * shader.c - OpenGL shader for scanline-snapped rendering
 * 
 * This shader snaps vertical pixels to scanlines while allowing
 * smooth or 1:1 horizontal stretching. Inspired by RetroArch CRT shaders.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

static GLuint program = 0;
static GLuint vbo = 0;

// Vertex shader
static const char *vert_src = 
    "#version 120\n"
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n"
    "}\n";

// Fragment shader - scanline snapping
static const char *frag_src = 
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 src_size;\n"
    "uniform vec2 dst_size;\n"
    "uniform vec4 crop;\n"           // x, y, w, h in source pixels
    "uniform float h_stretch;\n"
    "uniform int smooth_h;\n"
    "varying vec2 v_texcoord;\n"
    "\n"
    "void main() {\n"
    "    // Map output coordinates to cropped source region\n"
    "    vec2 uv = v_texcoord;\n"
    "    \n"
    "    // Apply horizontal stretch centered\n"
    "    float center = 0.5;\n"
    "    uv.x = center + (uv.x - center) / h_stretch;\n"
    "    \n"
    "    // Check bounds\n"
    "    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n"
    "        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    \n"
    "    // Map to crop region\n"
    "    vec2 src_uv;\n"
    "    src_uv.x = (crop.x + uv.x * crop.z) / src_size.x;\n"
    "    src_uv.y = (crop.y + uv.y * crop.w) / src_size.y;\n"
    "    \n"
    "    // Vertical: snap to nearest source pixel (scanline)\n"
    "    float src_y_pixel = uv.y * crop.w;\n"
    "    float snapped_y = floor(src_y_pixel + 0.5) / crop.w;\n"
    "    src_uv.y = (crop.y + snapped_y * crop.w) / src_size.y;\n"
    "    \n"
    "    // Horizontal: smooth or 1:1 based on setting\n"
    "    if (smooth_h == 0) {\n"
    "        // 1:1 - snap to nearest pixel\n"
    "        float src_x_pixel = uv.x * crop.z;\n"
    "        float snapped_x = floor(src_x_pixel + 0.5) / crop.z;\n"
    "        src_uv.x = (crop.x + snapped_x * crop.z) / src_size.x;\n"
    "    }\n"
    "    // else: use bilinear filtered x\n"
    "    \n"
    "    gl_FragColor = texture2D(tex, src_uv);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

bool shader_init(void) {
    GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    
    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return false;
    }
    
    program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);
    
    glDeleteShader(vert);
    glDeleteShader(frag);
    
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "Shader link error: %s\n", log);
        glDeleteProgram(program);
        program = 0;
        return false;
    }
    
    // Create VBO for fullscreen quad
    // Position (x,y) + Texcoord (u,v)
    float vertices[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right
        -1.0f,  1.0f,  0.0f, 0.0f,  // top-left
         1.0f,  1.0f,  1.0f, 0.0f,  // top-right
    };
    
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    return true;
}

void shader_cleanup(void) {
    if (vbo) {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (program) {
        glDeleteProgram(program);
        program = 0;
    }
}

void shader_render(GLuint texture, const config_t *config,
                   int src_width, int src_height,
                   int dst_width, int dst_height) {
    glUseProgram(program);
    
    // Set uniforms
    glUniform2f(glGetUniformLocation(program, "src_size"), 
                (float)src_width, (float)src_height);
    glUniform2f(glGetUniformLocation(program, "dst_size"),
                (float)dst_width, (float)dst_height);
    glUniform4f(glGetUniformLocation(program, "crop"),
                (float)config->crop_x, (float)config->crop_y,
                (float)config->crop_w, (float)config->crop_h);
    glUniform1f(glGetUniformLocation(program, "h_stretch"), config->h_stretch);
    glUniform1i(glGetUniformLocation(program, "smooth_h"), config->smooth_h ? 1 : 0);
    
    // Bind texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);
    
    // Set texture filtering based on mode
    if (config->smooth_h) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    
    // Draw fullscreen quad
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    
    GLint pos_loc = glGetAttribLocation(program, "position");
    GLint tex_loc = glGetAttribLocation(program, "texcoord");
    
    glEnableVertexAttribArray(pos_loc);
    glEnableVertexAttribArray(tex_loc);
    
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    glDisableVertexAttribArray(pos_loc);
    glDisableVertexAttribArray(tex_loc);
    
    glUseProgram(0);
}
