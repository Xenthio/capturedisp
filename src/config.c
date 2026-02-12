/*
 * config.c - Configuration and preset management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "config.h"

#define CONFIG_DIR ".config/capturedisp"
#define PRESETS_DIR "presets"
#define MAIN_CONFIG "config.ini"

static char *get_config_dir(void) {
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/%s", home, CONFIG_DIR);
    return path;
}

static char *get_presets_dir(void) {
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", get_config_dir(), PRESETS_DIR);
    return path;
}

static void ensure_config_dirs(void) {
    char *config_dir = get_config_dir();
    char *presets_dir = get_presets_dir();
    
    mkdir(config_dir, 0755);
    mkdir(presets_dir, 0755);
}

void config_init(config_t *config) {
    memset(config, 0, sizeof(*config));
    config->crop_x = 0;
    config->crop_y = 0;
    config->crop_w = 0;  // Will be set to source size
    config->crop_h = 0;
    config->h_stretch = 1.0f;
    config->smooth_h = true;
    config->use_240p = true;  // Default to 240p for retro games
    config->scanline_offset = 0;
}

static bool parse_config_file(config_t *config, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], value[128];
        if (sscanf(line, "%63[^=]=%127s", key, value) == 2) {
            if (strcmp(key, "crop_x") == 0) config->crop_x = atoi(value);
            else if (strcmp(key, "crop_y") == 0) config->crop_y = atoi(value);
            else if (strcmp(key, "crop_w") == 0) config->crop_w = atoi(value);
            else if (strcmp(key, "crop_h") == 0) config->crop_h = atoi(value);
            else if (strcmp(key, "h_stretch") == 0) config->h_stretch = atof(value);
            else if (strcmp(key, "smooth_h") == 0) config->smooth_h = atoi(value) != 0;
            else if (strcmp(key, "use_240p") == 0) config->use_240p = atoi(value) != 0;
            else if (strcmp(key, "scanline_offset") == 0) config->scanline_offset = atoi(value);
        }
    }
    
    fclose(f);
    return true;
}

static bool write_config_file(const config_t *config, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    
    fprintf(f, "crop_x=%d\n", config->crop_x);
    fprintf(f, "crop_y=%d\n", config->crop_y);
    fprintf(f, "crop_w=%d\n", config->crop_w);
    fprintf(f, "crop_h=%d\n", config->crop_h);
    fprintf(f, "h_stretch=%f\n", config->h_stretch);
    fprintf(f, "smooth_h=%d\n", config->smooth_h ? 1 : 0);
    fprintf(f, "use_240p=%d\n", config->use_240p ? 1 : 0);
    fprintf(f, "scanline_offset=%d\n", config->scanline_offset);
    
    fclose(f);
    return true;
}

bool config_load(config_t *config) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", get_config_dir(), MAIN_CONFIG);
    return parse_config_file(config, path);
}

bool config_save(const config_t *config) {
    ensure_config_dirs();
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", get_config_dir(), MAIN_CONFIG);
    return write_config_file(config, path);
}

bool config_load_preset(config_t *config, const char *name) {
    // Built-in presets
    if (strcmp(name, "NES-Switch-1080p") == 0 || strcmp(name, "nes-switch-1080p") == 0) {
        config->crop_x = 448;
        config->crop_y = 83;
        config->crop_w = 1024;
        config->crop_h = 912;
        config->h_stretch = 1.0f;
        config->smooth_h = true;
        config->use_240p = true;
        config->scanline_offset = 0;
        return true;
    }
    
    if (strcmp(name, "SNES-Switch-1080p") == 0 || strcmp(name, "snes-switch-1080p") == 0) {
        config->crop_x = 448;
        config->crop_y = 92;
        config->crop_w = 1024;
        config->crop_h = 896;
        config->h_stretch = 1.0f;
        config->smooth_h = true;
        config->use_240p = true;
        config->scanline_offset = 0;
        return true;
    }
    
    // Load from file
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ini", get_presets_dir(), name);
    return parse_config_file(config, path);
}

bool config_save_preset(const config_t *config, const char *name) {
    ensure_config_dirs();
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ini", get_presets_dir(), name);
    bool ok = write_config_file(config, path);
    if (ok) {
        printf("Preset saved: %s\n", name);
    }
    return ok;
}

int config_list_presets(char ***names) {
    DIR *dir = opendir(get_presets_dir());
    if (!dir) {
        *names = NULL;
        return 0;
    }
    
    // Count .ini files
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (strstr(ent->d_name, ".ini")) count++;
    }
    
    if (count == 0) {
        closedir(dir);
        *names = NULL;
        return 0;
    }
    
    // Allocate array
    *names = malloc(count * sizeof(char*));
    
    // Read names
    rewinddir(dir);
    int i = 0;
    while ((ent = readdir(dir)) && i < count) {
        char *dot = strstr(ent->d_name, ".ini");
        if (dot) {
            int len = dot - ent->d_name;
            (*names)[i] = malloc(len + 1);
            strncpy((*names)[i], ent->d_name, len);
            (*names)[i][len] = '\0';
            i++;
        }
    }
    
    closedir(dir);
    return i;
}
