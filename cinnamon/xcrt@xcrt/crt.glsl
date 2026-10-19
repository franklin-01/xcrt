// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

// xcrt CRT shader for Cinnamon, injected as a Cogl texture-lookup snippet.
// Same look as shaders/crt.frag in the standalone compositor, rewritten for
// GLSL 1.10 and a single pass: bloom is sampled from rings around the pixel,
// and there is no phosphor persistence (that needs the previous frame).
// Texture coordinates have their origin at the top-left of the stage.

uniform vec2  xcrt_stage;       // stage size in pixels
uniform vec4  xcrt_mons[8];     // monitor rects: x, y, width, height
uniform float xcrt_nmons;
uniform float xcrt_time;
uniform float xcrt_frame;

uniform vec2  xcrt_curvature;   // already multiplied by the curvature intensity
uniform float xcrt_corner;
uniform vec2  xcrt_scanline;    // strength, size in px
uniform vec3  xcrt_mask;        // type (0 none, 1 aperture, 2 slot, 3 shadow), strength, size in px
uniform vec2  xcrt_beam;        // softness px, chromatic aberration px
uniform vec2  xcrt_bloom;       // strength, radius
uniform float xcrt_vignette;
uniform vec3  xcrt_noise;       // noise, flicker, jitter px
uniform vec2  xcrt_roll;        // strength, speed
uniform vec4  xcrt_grade;       // brightness, contrast, saturation, gamma
uniform vec4  xcrt_tint;        // rgb, monochrome flag

// xcrt_hash is "Hash without Sine" by David Hoskins, https://www.shadertoy.com/view/4djSRW
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
float xcrt_hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec4 xcrt_monitor(vec2 px)
{
    for (int i = 0; i < 8; i++) {
        if (float(i) >= xcrt_nmons)
            break;
        vec4 m = xcrt_mons[i];
        if (px.x >= m.x && px.y >= m.y && px.x < m.x + m.z && px.y < m.y + m.w)
            return m;
    }
    return vec4(0.0, 0.0, xcrt_stage);
}

// Barrel distortion: each axis bows proportionally to the other axis squared.
vec2 xcrt_warp(vec2 uv)
{
    vec2 c = uv * 2.0 - 1.0;
    c *= 1.0 + xcrt_curvature * (c.yx * c.yx);
    return c * 0.5 + 0.5;
}

vec3 xcrt_scene(sampler2D tex, vec4 mon, vec2 q)
{
    return texture2D(tex, (mon.xy + q * mon.zw) / xcrt_stage).rgb;
}

vec3 xcrt_beam_at(sampler2D tex, vec4 mon, vec2 q)
{
    if (xcrt_beam.x <= 0.0)
        return xcrt_scene(tex, mon, q);
    vec2 d = vec2(xcrt_beam.x / mon.z, 0.0);
    return xcrt_scene(tex, mon, q) * 0.5 + (xcrt_scene(tex, mon, q - d) + xcrt_scene(tex, mon, q + d)) * 0.25;
}

vec3 xcrt_grade_color(vec3 c)
{
    const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
    c = mix(vec3(dot(c, luma)), c, xcrt_grade.z);
    c = max((c - 0.5) * xcrt_grade.y + 0.5, 0.0);
    c = (xcrt_tint.w > 0.5) ? vec3(dot(c, luma)) * xcrt_tint.rgb : c * xcrt_tint.rgb;
    return pow(c, vec3(xcrt_grade.w));
}

// Glow: average of two rings of samples (golden-angle spiral).
vec3 xcrt_glow(sampler2D tex, vec4 mon, vec2 q)
{
    vec2 r = vec2(7.0 * xcrt_bloom.y) / mon.zw;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        float a = float(i) * 2.39996;
        float d = (i < 6) ? 1.0 : 2.2;
        sum += xcrt_scene(tex, mon, q + vec2(cos(a), sin(a)) * r * d);
    }
    return sum / 12.0;
}

// Phosphor mask in physical pixels.
vec3 xcrt_mask_at(vec2 px)
{
    float type = xcrt_mask.x;
    if (type < 0.5 || xcrt_mask.y <= 0.0)
        return vec3(1.0);
    float s = max(xcrt_mask.z, 1.0);
    float dark = 1.0 - xcrt_mask.y;
    vec2 p = floor(px);
    float x = p.x;
    float gap = 1.0;
    if (type > 1.5 && type < 2.5) {
        // slot mask: triads staggered vertically with thin horizontal gaps
        float slot_h = max(floor(s * 1.5 + 0.5), 3.0);
        float y = p.y + mod(floor(x / s), 2.0) * floor(slot_h * 0.5);
        if (mod(y, slot_h) < 1.0)
            gap = dark;
    } else if (type > 2.5) {
        // shadow (dot) mask: every other row shifted by half a triad
        float row_h = max(floor(s * 0.5 + 0.5), 1.0);
        x += mod(floor(p.y / row_h), 2.0) * floor(s * 0.5);
    }
    float i = floor(mod(x, s) / s * 3.0);
    vec3 m = vec3(dark);
    if (i < 0.5)
        m.r = 1.0;
    else if (i < 1.5)
        m.g = 1.0;
    else
        m.b = 1.0;
    return m * gap;
}

vec4 xcrt_crt(sampler2D tex, vec2 tc)
{
    vec2 px = tc * xcrt_stage;
    vec4 mon = xcrt_monitor(px);
    vec2 q = xcrt_warp((px - mon.xy) / mon.zw);
    float line_px = max(xcrt_scanline.y, 1.0);

    if (xcrt_noise.z > 0.0) {
        float line = floor(q.y * mon.w / line_px);
        float j = xcrt_hash(vec2(line, floor(xcrt_time * 30.0))) - 0.5;
        q.x += j * j * j * 8.0 * xcrt_noise.z / mon.z;  // mostly small, occasional kicks
    }

    vec3 col;
    if (xcrt_beam.y > 0.0) {
        vec2 ca = (q - 0.5) * 2.0 * xcrt_beam.y / mon.zw;
        col = vec3(xcrt_beam_at(tex, mon, q + ca).r, xcrt_beam_at(tex, mon, q).g, xcrt_beam_at(tex, mon, q - ca).b);
    } else {
        col = xcrt_beam_at(tex, mon, q);
    }
    col = xcrt_grade_color(col);
    // bright areas widen the beam and wash out the mask, like a real tube
    float lum = clamp(max(col.r, max(col.g, col.b)), 0.0, 1.0);

    if (xcrt_scanline.x > 0.0) {
        float yl = (q.y * mon.w - 0.5) / line_px;
        float pos = fract(yl + 0.5) - 0.5;
        float width = mix(0.20, 0.50, lum);
        float b = exp(-0.5 * pos * pos / (width * width));
        col *= mix(1.0, b, xcrt_scanline.x);
    }

    col *= mix(xcrt_mask_at(px), vec3(1.0), 0.4 * lum);
    col *= xcrt_grade.x;

    if (xcrt_bloom.x > 0.0) {
        vec3 glow = xcrt_grade_color(xcrt_glow(tex, mon, q)) * xcrt_bloom.x;
        col = 1.0 - (1.0 - clamp(col, 0.0, 1.0)) * (1.0 - clamp(glow, 0.0, 1.0));
    }

    if (xcrt_vignette > 0.0) {
        float v = clamp(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.0, 1.0);
        col *= mix(1.0, pow(v, 0.25), xcrt_vignette);
    }
    if (xcrt_roll.x > 0.0) {
        float band = 0.5 + 0.5 * sin((q.y + xcrt_time * xcrt_roll.y) * 6.2831853);
        col *= 1.0 - xcrt_roll.x * smoothstep(0.3, 1.0, band);
    }
    if (xcrt_noise.y > 0.0)
        col *= 1.0 - xcrt_noise.y * xcrt_hash(vec2(xcrt_frame, 17.0));
    if (xcrt_noise.x > 0.0)
        col += (xcrt_hash(px + fract(xcrt_time * 7.13) * 1000.0) - 0.5) * xcrt_noise.x;

    // tube edge with rounded corners, antialiased
    vec2 p = (q - 0.5) * mon.zw;
    float r = xcrt_corner * min(mon.z, mon.w);
    vec2 d = abs(p) - (0.5 * mon.zw - r);
    float dist = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
    col *= clamp(0.5 - dist, 0.0, 1.0);

    return vec4(clamp(col, 0.0, 1.0), 1.0);
}
