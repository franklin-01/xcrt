// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

#pragma once

#include <stdbool.h>
#include <stddef.h>

enum mask_type { MASK_NONE, MASK_APERTURE, MASK_SLOT, MASK_SHADOW };

typedef struct {
    char  name[32];
    float curvature_x, curvature_y; /* barrel distortion per axis */
    float corner_radius;            /* fraction of the shorter monitor side */
    float scanline_strength;        /* 0 = off, 1 = black gaps */
    float scanline_size;            /* pixels per scanline */
    int   mask_type;                /* enum mask_type */
    float mask_strength;
    float mask_size;                /* pixels per RGB triad */
    float softness;                 /* horizontal beam blur, pixels */
    float chroma;                   /* chromatic aberration at the edges, pixels */
    float bloom, bloom_radius;
    float vignette;
    float noise, flicker, jitter;
    float roll, roll_speed;         /* rolling hum bar */
    float brightness, contrast, saturation, gamma;
    bool  monochrome;
    float tint[3];
    float persistence;              /* phosphor decay per frame, 0..0.98 */
} crt_preset;

typedef struct {
    unsigned int  mods;
    unsigned long keysym;
} hotkey;

typedef struct {
    crt_preset *presets;
    int         npresets;
    int         current;
    char        start_preset[32];
    bool        draw_cursor;
    bool        vsync;
    bool        start_enabled;
    float       background[3]; /* used when no wallpaper pixmap is set */
    float       curvature_scale; /* multiplier on every preset's curvature */
    float       curvature_step;
    hotkey      toggle_key, next_key, prev_key;
    hotkey      curv_up_key, curv_down_key, curv_reset_key;
} xcrt_config;

#define CURVATURE_SCALE_MAX 3.0f

void config_init(xcrt_config *c);
void config_free(xcrt_config *c);
/* Loads a config file on top of the built-ins. Returns false only on I/O error. */
bool config_load_file(xcrt_config *c, const char *path);
/* Resolves the default user config path into buf; returns buf. */
const char *config_default_path(char *buf, int len);
int  config_find_preset(const xcrt_config *c, const char *name);
bool config_parse_hotkey(const char *s, hotkey *k);
/* Parses "1.5" or "150%" into 1.5. A leading sign is kept, for relative steps. */
bool config_parse_scale(const char *s, float *out);
/* True when the preset needs continuous redraws (noise, flicker, ...). */
bool preset_is_animated(const crt_preset *p);

/* Live editing. Keys are the config file keys (bloom, mask, tint, ...). */
bool preset_set_value(xcrt_config *c, crt_preset *p, const char *key, const char *val);
bool preset_get_float(const crt_preset *p, const char *key, float *out);
bool preset_equal(const crt_preset *a, const crt_preset *b);
/* Writes every parameter as "key = value" lines; returns the length needed. */
size_t preset_format(const crt_preset *p, char *buf, size_t len);
crt_preset *config_get_or_add_preset(xcrt_config *c, const char *name);
/* Writes the preset section and the [general] preset/curvature_scale keys into
 * the config file, keeping everything else in it. */
bool config_save(const char *path, const crt_preset *p, float curvature_scale);
