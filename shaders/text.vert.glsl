#version 450

layout(set = 0, binding = 0) uniform UniformBuffer {
    vec2 screen_size;
    vec2 scroll_offset;
    float time;
} ubo;

// Per-instance data
layout(location = 0) in vec2 in_pos;       // top-left position in pixels
layout(location = 1) in vec2 in_size;      // glyph size in pixels
layout(location = 2) in vec2 in_uv_min;    // UV top-left in atlas
layout(location = 3) in vec2 in_uv_max;    // UV bottom-right in atlas
layout(location = 4) in vec4 in_color;     // text color

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

const vec2 QUAD[6] = vec2[](
    vec2(0, 0), vec2(1, 0), vec2(1, 1),
    vec2(0, 0), vec2(1, 1), vec2(0, 1)
);

void main() {
    vec2 corner = QUAD[gl_VertexIndex];
    vec2 pixel_pos = in_pos + corner * in_size - ubo.scroll_offset;
    vec2 ndc = (pixel_pos / ubo.screen_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);
    frag_uv = mix(in_uv_min, in_uv_max, corner);
    frag_color = in_color;
}
