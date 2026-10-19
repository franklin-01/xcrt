#version 330 core
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro
// Phosphor persistence: bright pixels fade out over several frames.
in vec2 v_uv;
out vec4 frag;

uniform sampler2D u_cur;
uniform sampler2D u_prev;
uniform float u_decay;

void main()
{
    vec3 cur = texture(u_cur, v_uv).rgb;
    vec3 prev = texture(u_prev, v_uv).rgb * u_decay;
    frag = vec4(max(cur, prev), 1.0);
}
