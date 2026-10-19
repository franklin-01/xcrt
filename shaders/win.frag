#version 330 core
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro
in vec2 v_tc;
out vec4 frag;

uniform sampler2D u_tex;
uniform int u_opaque;                 // depth-24 windows carry garbage alpha

void main()
{
    vec4 c = texture(u_tex, v_tc);
    if (u_opaque == 1)
        c.a = 1.0;
    frag = c;                         // premultiplied alpha
}
