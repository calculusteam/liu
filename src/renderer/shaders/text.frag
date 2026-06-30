#version 330 core

in vec2 v_uv;
in vec4 v_fg;
in vec4 v_bg;

uniform sampler2D u_atlas;

out vec4 frag_color;

void main() {
    float alpha = texture(u_atlas, v_uv).r;
    frag_color = mix(v_bg, v_fg, alpha);
}
