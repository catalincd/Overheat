#version 450

layout(set = 0, binding = 0) uniform UniformBuffer {
    vec2 screen_size;
    vec2 scroll_offset;
    float time;
} ubo;

// Per-instance data
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_size;
layout(location = 2) in vec4 in_color;
layout(location = 3) in float in_corner_radius;
layout(location = 4) in float in_border_width;
layout(location = 5) in vec4 in_border_color;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec2 frag_local_pos;
layout(location = 2) out vec2 frag_size;
layout(location = 3) out float frag_corner_radius;
layout(location = 4) out float frag_border_width;
layout(location = 5) out vec4 frag_border_color;

const vec2 QUAD[6] = vec2[](
    vec2(0, 0), vec2(1, 0), vec2(1, 1),
    vec2(0, 0), vec2(1, 1), vec2(0, 1)
);

void main() {
    vec2 corner = QUAD[gl_VertexIndex];
    vec2 pixel_pos = in_pos + corner * in_size;
    vec2 ndc = (pixel_pos / ubo.screen_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);
    frag_color = in_color;
    frag_local_pos = corner * in_size;
    frag_size = in_size;
    frag_corner_radius = in_corner_radius;
    frag_border_width = in_border_width;
    frag_border_color = in_border_color;
}
