#version 330 core
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro
// Draws a window (or wallpaper/cursor) quad given in X screen pixels.
layout(location = 0) in vec2 a_pos;   // screen pixels, top-left origin
layout(location = 1) in vec2 a_tc;    // texture pixels, top-left origin

uniform vec2 u_screen;
uniform vec2 u_texsize;
uniform int  u_flip;                  // 1 when texture row 0 is the bottom

out vec2 v_tc;

void main()
{
    vec2 n = a_pos / u_screen;
    gl_Position = vec4(n.x * 2.0 - 1.0, 1.0 - n.y * 2.0, 0.0, 1.0);
    vec2 t = a_tc / u_texsize;
    if (u_flip == 1)
        t.y = 1.0 - t.y;
    v_tc = t;
}
