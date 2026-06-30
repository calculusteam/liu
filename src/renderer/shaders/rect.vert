#version 330 core

// Per-vertex: position(2) + color(4)
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;

uniform mat4 u_projection;

out vec4 v_color;

void main() {
    gl_Position = u_projection * vec4(a_pos, 0.0, 1.0);
    v_color = a_color;
}
