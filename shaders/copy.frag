#version 330 core
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro
// Passthrough used while the effect is toggled off.
in vec2 v_uv;
out vec4 frag;

uniform sampler2D u_scene;
uniform vec4 u_mon;

void main()
{
    frag = vec4(texture(u_scene, u_mon.xy + v_uv * u_mon.zw).rgb, 1.0);
}
