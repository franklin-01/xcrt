#version 330 core
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro
// Separable 9-tap gaussian used for bloom/halation.
in vec2 v_uv;
out vec4 frag;

uniform sampler2D u_src;
uniform vec2  u_dir;                  // step per tap, in uv units
uniform float u_lod;                  // mip level to read (downsampling)

const float W[5] = float[](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);

void main()
{
    vec3 c = textureLod(u_src, v_uv, u_lod).rgb * W[0];
    for (int i = 1; i < 5; i++) {
        c += textureLod(u_src, v_uv + u_dir * float(i), u_lod).rgb * W[i];
        c += textureLod(u_src, v_uv - u_dir * float(i), u_lod).rgb * W[i];
    }
    frag = vec4(c, 1.0);
}
