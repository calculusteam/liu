#version 330 core

// Per-vertex: position(2) + texcoord(2) + fg_color(4) + bg_color(4)
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_fg;
layout(location = 3) in vec4 a_bg;

uniform mat4 u_projection;

out vec2 v_uv;
out vec4 v_fg;
out vec4 v_bg;

void main() {
    gl_Position = u_projection * vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
    v_fg = a_fg;
    v_bg = a_bg;
}
