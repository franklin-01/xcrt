// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

#include "config.h"

#include <X11/Xlib.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built-in presets use the same syntax as the user config file, so a
 * [preset NAME] section in xcrt.conf can override any value below. */
static const char builtin_presets[] =
    "[preset arcade]\n"
    "curvature = 0.10\n"
    "corner_radius = 0.03\n"
    "scanline_strength = 0.55\n"
    "scanline_size = 4\n"
    "mask = slot\n"
    "mask_strength = 0.30\n"
    "mask_size = 3\n"
    "softness = 0.6\n"
    "chroma = 0.8\n"
    "bloom = 0.35\n"
    "vignette = 0.35\n"
    "noise = 0.02\n"
    "flicker = 0.015\n"
    "brightness = 1.25\n"
    "saturation = 1.15\n"
    "\n"
    "[preset pc]\n"
    "curvature = 0.025\n"
    "corner_radius = 0.012\n"
    "scanline_strength = 0.25\n"
    "scanline_size = 3\n"
    "mask = aperture\n"
    "mask_strength = 0.12\n"
    "mask_size = 3\n"
    "softness = 0.25\n"
    "chroma = 0.25\n"
    "bloom = 0.10\n"
    "vignette = 0.15\n"
    "brightness = 1.08\n"
    "\n"
    "[preset trinitron]\n"
    "curvature_x = 0.05\n"
    "curvature_y = 0.0\n"
    "corner_radius = 0.005\n"
    "scanline_strength = 0.40\n"
    "scanline_size = 3\n"
    "mask = aperture\n"
    "mask_strength = 0.35\n"
    "mask_size = 3\n"
    "softness = 0.35\n"
    "chroma = 0.3\n"
    "bloom = 0.25\n"
    "vignette = 0.20\n"
    "brightness = 1.22\n"
    "saturation = 1.10\n"
    "\n"
    "[preset tv]\n"
    "curvature = 0.14\n"
    "corner_radius = 0.05\n"
    "scanline_strength = 0.50\n"
    "scanline_size = 4\n"
    "mask = shadow\n"
    "mask_strength = 0.30\n"
    "mask_size = 4\n"
    "softness = 1.2\n"
    "chroma = 1.6\n"
    "bloom = 0.45\n"
    "vignette = 0.50\n"
    "noise = 0.07\n"
    "flicker = 0.04\n"
    "jitter = 0.6\n"
    "roll = 0.06\n"
    "roll_speed = 0.08\n"
    "brightness = 1.30\n"
    "saturation = 1.25\n"
    "contrast = 1.05\n"
    "\n"
    "[preset green]\n"
    "curvature = 0.07\n"
    "corner_radius = 0.03\n"
    "scanline_strength = 0.45\n"
    "scanline_size = 3\n"
    "mask = none\n"
    "softness = 0.5\n"
    "bloom = 0.55\n"
    "bloom_radius = 1.3\n"
    "vignette = 0.30\n"
    "noise = 0.025\n"
    "flicker = 0.02\n"
    "monochrome = true\n"
    "tint = #33ff66\n"
    "gamma = 1.1\n"
    "brightness = 1.20\n"
    "persistence = 0.80\n"
    "\n"
    "[preset amber]\n"
    "base = green\n"
    "tint = #ffb000\n"
    "persistence = 0.65\n";

void config_init(xcrt_config *c)
{
    memset(c, 0, sizeof(*c));
    c->draw_cursor = true;
    c->vsync = true;
    c->start_enabled = true;
    snprintf(c->start_preset, sizeof(c->start_preset), "pc");
    c->curvature_scale = 1.0f;
    c->curvature_step = 0.1f;
    config_parse_hotkey("Super+Shift+F12", &c->toggle_key);
    config_parse_hotkey("Super+Shift+F11", &c->next_key);
    config_parse_hotkey("Super+Shift+F10", &c->prev_key);
    config_parse_hotkey("Super+Shift+Page_Up", &c->curv_up_key);
    config_parse_hotkey("Super+Shift+Page_Down", &c->curv_down_key);
    config_parse_hotkey("Super+Shift+Home", &c->curv_reset_key);
}

void config_free(xcrt_config *c)
{
    free(c->presets);
    c->presets = NULL;
    c->npresets = 0;
}

static void preset_neutral(crt_preset *p, const char *name)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->scanline_size = 3.0f;
    p->mask_size = 3.0f;
    p->bloom_radius = 1.0f;
    p->roll_speed = 0.1f;
    p->brightness = 1.0f;
    p->contrast = 1.0f;
    p->saturation = 1.0f;
    p->gamma = 1.0f;
    p->tint[0] = p->tint[1] = p->tint[2] = 1.0f;
}

int config_find_preset(const xcrt_config *c, const char *name)
{
    for (int i = 0; i < c->npresets; i++)
        if (strcmp(c->presets[i].name, name) == 0)
            return i;
    return -1;
}

crt_preset *config_get_or_add_preset(xcrt_config *c, const char *name)
{
    int i = config_find_preset(c, name);
    if (i >= 0)
        return &c->presets[i];
    crt_preset *n = realloc(c->presets, sizeof(*n) * (c->npresets + 1));
    if (!n)
        return NULL;
    c->presets = n;
    preset_neutral(&c->presets[c->npresets], name);
    return &c->presets[c->npresets++];
}

bool preset_is_animated(const crt_preset *p)
{
    return p->noise > 0 || p->flicker > 0 || p->jitter > 0 || p->roll > 0;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static bool parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_float(const char *v, float *out)
{
    char *end;
    float f = strtof(v, &end);
    if (end == v || *trim(end) != '\0')
        return false;
    *out = f;
    return true;
}

bool config_parse_scale(const char *s, float *out)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    char *v = trim(buf);
    size_t n = strlen(v);
    bool percent = n > 0 && v[n - 1] == '%';
    if (percent)
        v[n - 1] = '\0';
    if (!parse_float(v, out))
        return false;
    if (percent)
        *out /= 100.0f;
    return true;
}

/* Accepts "#rrggbb" or three floats "r g b". */
static bool parse_color(const char *v, float rgb[3])
{
    if (v[0] == '#' && strlen(v) == 7) {
        unsigned int hex;
        if (sscanf(v + 1, "%x", &hex) != 1)
            return false;
        rgb[0] = ((hex >> 16) & 0xff) / 255.0f;
        rgb[1] = ((hex >> 8) & 0xff) / 255.0f;
        rgb[2] = (hex & 0xff) / 255.0f;
        return true;
    }
    return sscanf(v, "%f %f %f", &rgb[0], &rgb[1], &rgb[2]) == 3;
}

bool config_parse_hotkey(const char *s, hotkey *k)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    unsigned int mods = 0;
    KeySym sym = NoSymbol;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        tok = trim(tok);
        if (!strcasecmp(tok, "Shift"))
            mods |= ShiftMask;
        else if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control"))
            mods |= ControlMask;
        else if (!strcasecmp(tok, "Alt") || !strcasecmp(tok, "Mod1"))
            mods |= Mod1Mask;
        else if (!strcasecmp(tok, "Super") || !strcasecmp(tok, "Win") || !strcasecmp(tok, "Mod4"))
            mods |= Mod4Mask;
        else if ((sym = XStringToKeysym(tok)) == NoSymbol)
            return false;
    }
    if (sym == NoSymbol)
        return false;
    k->mods = mods;
    k->keysym = sym;
    return true;
}

#define PF(field) { #field, offsetof(crt_preset, field) }
static const struct {
    const char *key;
    size_t      off;
} float_keys[] = {
    PF(curvature_x), PF(curvature_y), PF(corner_radius),
    PF(scanline_strength), PF(scanline_size),
    PF(mask_strength), PF(mask_size),
    PF(softness), PF(chroma), PF(bloom), PF(bloom_radius), PF(vignette),
    PF(noise), PF(flicker), PF(jitter), PF(roll), PF(roll_speed),
    PF(brightness), PF(contrast), PF(saturation), PF(gamma), PF(persistence),
};
#undef PF

static bool set_preset_key(xcrt_config *c, crt_preset *p, const char *key, const char *val)
{
    for (size_t i = 0; i < sizeof(float_keys) / sizeof(float_keys[0]); i++)
        if (!strcmp(key, float_keys[i].key))
            return parse_float(val, (float *)((char *)p + float_keys[i].off));

    if (!strcmp(key, "curvature")) {
        if (!parse_float(val, &p->curvature_x))
            return false;
        p->curvature_y = p->curvature_x;
        return true;
    }
    if (!strcmp(key, "mask") || !strcmp(key, "mask_type")) {
        static const char *names[] = { "none", "aperture", "slot", "shadow" };
        for (int i = 0; i < 4; i++)
            if (!strcasecmp(val, names[i]) || (val[0] == '0' + i && !val[1])) {
                p->mask_type = i;
                return true;
            }
        return false;
    }
    if (!strcmp(key, "monochrome"))
        return parse_bool(val, &p->monochrome);
    if (!strcmp(key, "tint"))
        return parse_color(val, p->tint);
    if (!strcmp(key, "base")) {
        int b = config_find_preset(c, val);
        if (b < 0)
            return false;
        char name[sizeof(p->name)];
        memcpy(name, p->name, sizeof(name));
        *p = c->presets[b];
        memcpy(p->name, name, sizeof(name));
        return true;
    }
    return false;
}

#define NFLOAT_KEYS (sizeof(float_keys) / sizeof(float_keys[0]))
static const char *mask_names[] = { "none", "aperture", "slot", "shadow" };

static float *float_field(crt_preset *p, size_t i)
{
    return (float *)((char *)p + float_keys[i].off);
}

bool preset_set_value(xcrt_config *c, crt_preset *p, const char *key, const char *val)
{
    return set_preset_key(c, p, key, val);
}

bool preset_get_float(const crt_preset *p, const char *key, float *out)
{
    if (!strcmp(key, "curvature"))
        key = "curvature_x";
    for (size_t i = 0; i < NFLOAT_KEYS; i++)
        if (!strcmp(key, float_keys[i].key)) {
            *out = *float_field((crt_preset *)p, i);
            return true;
        }
    return false;
}

bool preset_equal(const crt_preset *a, const crt_preset *b)
{
    for (size_t i = 0; i < NFLOAT_KEYS; i++)
        if (*float_field((crt_preset *)a, i) != *float_field((crt_preset *)b, i))
            return false;
    return a->mask_type == b->mask_type && a->monochrome == b->monochrome &&
           !memcmp(a->tint, b->tint, sizeof(a->tint));
}

size_t preset_format(const crt_preset *p, char *buf, size_t len)
{
    size_t n = 0;
    char line[128];
    for (size_t i = 0; i <= NFLOAT_KEYS + 2; i++) {
        if (i < NFLOAT_KEYS)
            snprintf(line, sizeof(line), "%s = %g\n", float_keys[i].key, *float_field((crt_preset *)p, i));
        else if (i == NFLOAT_KEYS)
            snprintf(line, sizeof(line), "mask = %s\n", mask_names[p->mask_type & 3]);
        else if (i == NFLOAT_KEYS + 1)
            snprintf(line, sizeof(line), "monochrome = %s\n", p->monochrome ? "true" : "false");
        else /* three floats keep the colour exact */
            snprintf(line, sizeof(line), "tint = %g %g %g\n", p->tint[0], p->tint[1], p->tint[2]);
        size_t l = strlen(line);
        if (n + l < len)
            memcpy(buf + n, line, l + 1);
        n += l;
    }
    return n;
}

static bool set_general_key(xcrt_config *c, const char *key, const char *val)
{
    if (!strcmp(key, "preset")) {
        snprintf(c->start_preset, sizeof(c->start_preset), "%s", val);
        return true;
    }
    if (!strcmp(key, "draw_cursor"))
        return parse_bool(val, &c->draw_cursor);
    if (!strcmp(key, "vsync"))
        return parse_bool(val, &c->vsync);
    if (!strcmp(key, "enabled"))
        return parse_bool(val, &c->start_enabled);
    if (!strcmp(key, "background"))
        return parse_color(val, c->background);
    if (!strcmp(key, "curvature_scale")) {
        float s;
        if (!config_parse_scale(val, &s) || s < 0 || s > CURVATURE_SCALE_MAX)
            return false;
        c->curvature_scale = s;
        return true;
    }
    if (!strcmp(key, "curvature_step")) {
        float s;
        if (!config_parse_scale(val, &s) || s <= 0)
            return false;
        c->curvature_step = s;
        return true;
    }
    if (!strcmp(key, "curvature_up_key"))
        return config_parse_hotkey(val, &c->curv_up_key);
    if (!strcmp(key, "curvature_down_key"))
        return config_parse_hotkey(val, &c->curv_down_key);
    if (!strcmp(key, "curvature_reset_key"))
        return config_parse_hotkey(val, &c->curv_reset_key);
    if (!strcmp(key, "toggle_key"))
        return config_parse_hotkey(val, &c->toggle_key);
    if (!strcmp(key, "next_preset_key"))
        return config_parse_hotkey(val, &c->next_key);
    if (!strcmp(key, "prev_preset_key"))
        return config_parse_hotkey(val, &c->prev_key);
    return false;
}

/* Inline comments start with '#' or ';' after whitespace, so a value
 * beginning with '#' (a colour) survives. */
static void strip_comment(char *v)
{
    for (char *p = v + 1; *p; p++)
        if ((*p == '#' || *p == ';') && isspace((unsigned char)p[-1])) {
            *p = '\0';
            return;
        }
}

static void parse_text(xcrt_config *c, char *text, const char *origin)
{
    crt_preset *section = NULL; /* NULL means [general] */
    int ln = 0;
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        ln++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                fprintf(stderr, "xcrt: %s:%d: unterminated section\n", origin, ln);
                continue;
            }
            *end = '\0';
            char *name = trim(s + 1);
            if (!strcmp(name, "general")) {
                section = NULL;
            } else if (!strncmp(name, "preset", 6) && isspace((unsigned char)name[6])) {
                section = config_get_or_add_preset(c, trim(name + 6));
            } else {
                fprintf(stderr, "xcrt: %s:%d: unknown section [%s]\n", origin, ln, name);
                section = NULL;
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "xcrt: %s:%d: expected key = value\n", origin, ln);
            continue;
        }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        if (*val) {
            strip_comment(val);
            val = trim(val);
        }
        bool ok = section ? set_preset_key(c, section, key, val) : set_general_key(c, key, val);
        if (!ok)
            fprintf(stderr, "xcrt: %s:%d: invalid setting '%s = %s'\n", origin, ln, key, val);
    }
}

static void load_builtins(xcrt_config *c)
{
    char *text = strdup(builtin_presets);
    if (text) {
        parse_text(c, text, "<built-in>");
        free(text);
    }
}

const char *config_default_path(char *buf, int len)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(buf, len, "%s/xcrt/xcrt.conf", xdg);
    else
        snprintf(buf, len, "%s/.config/xcrt/xcrt.conf", getenv("HOME") ? getenv("HOME") : ".");
    return buf;
}

bool config_load_file(xcrt_config *c, const char *path)
{
    if (c->npresets == 0)
        load_builtins(c);
    if (!path)
        return true;

    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size + 1);
    if (!text) {
        fclose(f);
        return false;
    }
    size_t n = fread(text, 1, size, f);
    text[n] = '\0';
    fclose(f);
    parse_text(c, text, path);
    free(text);
    return true;
}

/* ------------------------------------------------------------------ */
/* saving                                                              */

typedef struct {
    char  *s;
    size_t n, cap;
} strbuf;

static void sb_add(strbuf *b, const char *s, size_t l)
{
    if (b->n + l + 1 > b->cap) {
        size_t cap = (b->n + l + 1) * 2;
        char *n = realloc(b->s, cap);
        if (!n)
            return;
        b->s = n;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, l);
    b->n += l;
    b->s[b->n] = '\0';
}

static void sb_str(strbuf *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

/* Collapses trailing blank lines so repeated saves do not accumulate them. */
static void sb_trim_blank(strbuf *b)
{
    while (b->n >= 2 && b->s[b->n - 1] == '\n' && b->s[b->n - 2] == '\n')
        b->s[--b->n] = '\0';
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    strbuf b = { 0 };
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        sb_add(&b, chunk, n);
    fclose(f);
    if (!b.s)
        sb_str(&b, "");
    return b.s;
}

static bool mkdir_parents(const char *path)
{
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return true;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    }
    return mkdir(dir, 0755) == 0 || errno == EEXIST;
}

/* "[preset NAME]" -> NAME, "[general]" -> "general"; NULL for other lines */
static bool section_name(const char *line, char *out, size_t len)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", line);
    char *s = trim(buf);
    if (*s != '[')
        return false;
    char *end = strchr(s, ']');
    if (!end)
        return false;
    *end = '\0';
    s = trim(s + 1);
    if (!strncmp(s, "preset", 6) && isspace((unsigned char)s[6]))
        s = trim(s + 6);
    else if (strcmp(s, "general"))
        s = "";
    snprintf(out, len, "%s%s", strcmp(s, "general") && *s ? "preset:" : "", s);
    return true;
}

static bool line_key_is(const char *line, const char *key)
{
    while (isspace((unsigned char)*line))
        line++;
    size_t l = strlen(key);
    if (strncmp(line, key, l))
        return false;
    line += l;
    while (*line == ' ' || *line == '\t')
        line++;
    return *line == '=';
}

bool config_save(const char *path, const crt_preset *p, float curvature_scale)
{
    char general[256], target[64], params[4096];
    snprintf(general, sizeof(general), "preset = %s\ncurvature_scale = %g%%\n", p->name,
             roundf(curvature_scale * 1000.0f) / 10.0f);
    snprintf(target, sizeof(target), "preset:%s", p->name);
    preset_format(p, params, sizeof(params));

    char *old = read_file(path);
    bool has_general = false;
    for (char *l = old; l && *l;) {
        size_t len = strcspn(l, "\n");
        char line[512], name[64];
        snprintf(line, sizeof(line), "%.*s", (int)(len < 511 ? len : 511), l);
        if (section_name(line, name, sizeof(name)) && !strcmp(name, "general"))
            has_general = true;
        l += len + (l[len] ? 1 : 0);
    }

    strbuf out = { 0 };
    bool in_general = !has_general; /* without a [general] header, the top of the file is general */
    bool general_pending = true, preset_written = false, skipping = false, top_has_keys = false;
    char section[64] = "";

    for (char *l = old; l && *l;) {
        size_t len = strcspn(l, "\n");
        char line[512];
        snprintf(line, sizeof(line), "%.*s", (int)(len < 511 ? len : 511), l);
        char *next = l[len] ? l + len + 1 : l + len;

        char name[64];
        if (section_name(line, name, sizeof(name))) {
            if (in_general && general_pending) {
                sb_trim_blank(&out);
                if (!has_general && !top_has_keys)
                    sb_str(&out, "[general]\n");
                sb_str(&out, general);
                sb_str(&out, "\n");
                general_pending = false;
            }
            snprintf(section, sizeof(section), "%s", name);
            in_general = !strcmp(name, "general");
            skipping = !strcmp(name, target);
            if (skipping) {
                if (!preset_written) {
                    sb_str(&out, line);
                    sb_str(&out, "\n");
                    sb_str(&out, params);
                    sb_str(&out, "\n");
                    preset_written = true;
                }
            } else {
                sb_str(&out, line);
                sb_str(&out, "\n");
            }
        } else if (!skipping) {
            bool replaced = in_general && (line_key_is(line, "preset") || line_key_is(line, "curvature_scale"));
            if (in_general && !has_general && strchr(line, '=') && *trim(line) != '#')
                top_has_keys = true;
            if (!replaced) {
                sb_str(&out, line);
                sb_str(&out, "\n");
            }
        }
        l = next;
    }
    if (general_pending) {
        sb_trim_blank(&out);
        if (!has_general && !top_has_keys)
            sb_str(&out, "[general]\n");
        sb_str(&out, general);
    }
    if (!preset_written) {
        char header[64];
        sb_trim_blank(&out);
        snprintf(header, sizeof(header), "%s[preset %s]\n", out.n ? "\n" : "", p->name);
        sb_str(&out, header);
        sb_str(&out, params);
    }
    sb_trim_blank(&out);
    free(old);

    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    bool ok = mkdir_parents(path);
    FILE *f = ok ? fopen(tmp, "w") : NULL;
    if (f) {
        ok = fwrite(out.s ? out.s : "", 1, out.n, f) == out.n;
        ok &= fclose(f) == 0;
        ok = ok && rename(tmp, path) == 0;
    } else {
        ok = false;
    }
    free(out.s);
    return ok;
}
