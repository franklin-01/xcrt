#version 330 core
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro
// CRT emulation, run once per monitor.
in vec2 v_uv;                         // 0..1 across the monitor, GL orientation
out vec4 frag;

uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform int   u_has_bloom;
uniform vec4  u_mon;                  // monitor rect in scene uv: offset.xy, size.zw
uniform vec2  u_mon_px;               // monitor size in pixels
uniform vec2  u_origin_px;            // monitor origin in framebuffer pixels
uniform float u_time;
uniform float u_frame;

uniform vec2  u_curvature;
uniform float u_corner_radius;
uniform float u_scanline_strength;
uniform float u_scanline_size;
uniform int   u_mask_type;            // 0 none, 1 aperture grille, 2 slot, 3 shadow
uniform float u_mask_strength;
uniform float u_mask_size;
uniform float u_softness;
uniform float u_chroma;
uniform float u_bloom_strength;
uniform float u_vignette;
uniform float u_noise;
uniform float u_flicker;
uniform float u_jitter;
uniform float u_roll;
uniform float u_roll_speed;
uniform float u_brightness;
uniform float u_contrast;
uniform float u_saturation;
uniform float u_gamma;
uniform int   u_monochrome;
uniform vec3  u_tint;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// hash12 is "Hash without Sine" by David Hoskins, https://www.shadertoy.com/view/4djSRW
//
// MIT License
// Copyright (c) 2014 David Hoskins.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Barrel distortion: each axis bows proportionally to the other axis squared.
vec2 warp(vec2 uv)
{
    vec2 c = uv * 2.0 - 1.0;
    c *= 1.0 + u_curvature * (c.yx * c.yx);
    return c * 0.5 + 0.5;
}

vec3 scene(vec2 q)
{
    return texture(u_scene, u_mon.xy + q * u_mon.zw).rgb;
}

// Horizontal beam spread.
vec3 beam(vec2 q)
{
    if (u_softness <= 0.0)
        return scene(q);
    vec2 d = vec2(u_softness / u_mon_px.x, 0.0);
    return scene(q) * 0.5 + (scene(q - d) + scene(q + d)) * 0.25;
}

vec3 grade(vec3 c)
{
    c = mix(vec3(dot(c, LUMA)), c, u_saturation);
    c = max((c - 0.5) * u_contrast + 0.5, 0.0);
    c = (u_monochrome == 1) ? vec3(dot(c, LUMA)) * u_tint : c * u_tint;
    return pow(c, vec3(u_gamma));
}

// Phosphor mask in physical pixels.
vec3 mask(vec2 px)
{
    if (u_mask_type == 0 || u_mask_strength <= 0.0)
        return vec3(1.0);
    float s = max(u_mask_size, 1.0);
    float dark = 1.0 - u_mask_strength;
    vec2 p = floor(px);
    float x = p.x;
    float gap = 1.0;
    if (u_mask_type == 2) {
        // slot mask: triads staggered vertically with thin horizontal gaps
        float slot_h = max(floor(s * 1.5 + 0.5), 3.0);
        float y = p.y + mod(floor(x / s), 2.0) * floor(slot_h * 0.5);
        if (mod(y, slot_h) < 1.0)
            gap = dark;
    } else if (u_mask_type == 3) {
        // shadow (dot) mask: every other row shifted by half a triad
        float row_h = max(floor(s * 0.5 + 0.5), 1.0);
        x += mod(floor(p.y / row_h), 2.0) * floor(s * 0.5);
    }
    int i = min(int(floor(mod(x, s) / s * 3.0)), 2);
    vec3 m = vec3(dark);
    m[i] = 1.0;
    return m * gap;
}

void main()
{
    vec2 q = warp(v_uv);
    float y_top = 1.0 - q.y;          // 0 at the top of the tube
    float line_px = max(u_scanline_size, 1.0);

    if (u_jitter > 0.0) {
        float line = floor(y_top * u_mon_px.y / line_px);
        float j = hash12(vec2(line, floor(u_time * 30.0))) - 0.5;
        q.x += j * j * j * 8.0 * u_jitter / u_mon_px.x;  // mostly small, occasional kicks
    }

    vec3 col;
    if (u_chroma > 0.0) {
        vec2 ca = (q - 0.5) * 2.0 * u_chroma / u_mon_px;
        col = vec3(beam(q + ca).r, beam(q).g, beam(q - ca).b);
    } else {
        col = beam(q);
    }
    col = grade(col);
    // Bright areas widen the beam and wash out the mask, like a real tube;
    // this keeps highlights bright without clipping away the structure.
    float lum = clamp(max(col.r, max(col.g, col.b)), 0.0, 1.0);

    if (u_scanline_strength > 0.0) {
        float yl = (y_top * u_mon_px.y - 0.5) / line_px;
        float pos = fract(yl + 0.5) - 0.5;            // distance from the beam centre
        float width = mix(0.20, 0.50, lum);
        float b = exp(-0.5 * pos * pos / (width * width));
        col *= mix(1.0, b, u_scanline_strength);
    }

    col *= mix(mask(gl_FragCoord.xy - u_origin_px), vec3(1.0), 0.4 * lum);
    col *= u_brightness;

    // screen blend: the glow fills dark gaps but cannot push highlights past white
    if (u_has_bloom == 1) {
        vec3 glow = grade(texture(u_bloom, u_mon.xy + q * u_mon.zw).rgb) * u_bloom_strength;
        col = 1.0 - (1.0 - clamp(col, 0.0, 1.0)) * (1.0 - clamp(glow, 0.0, 1.0));
    }

    if (u_vignette > 0.0) {
        float v = clamp(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.0, 1.0);
        col *= mix(1.0, pow(v, 0.25), u_vignette);
    }
    if (u_roll > 0.0) {
        float band = 0.5 + 0.5 * sin((y_top + u_time * u_roll_speed) * 6.2831853);
        col *= 1.0 - u_roll * smoothstep(0.3, 1.0, band);
    }
    if (u_flicker > 0.0)
        col *= 1.0 - u_flicker * hash12(vec2(u_frame, 17.0));

    if (u_noise > 0.0)
        col += (hash12(gl_FragCoord.xy + fract(u_time * 7.13) * 1000.0) - 0.5) * u_noise;

    // tube edge with rounded corners, antialiased
    vec2 p = (q - 0.5) * u_mon_px;
    float r = u_corner_radius * min(u_mon_px.x, u_mon_px.y);
    vec2 d = abs(p) - (0.5 * u_mon_px - r);
    float dist = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
    col *= clamp(0.5 - dist, 0.0, 1.0);

    frag = vec4(clamp(col, 0.0, 1.0), 1.0);
}
