#version 450
#extension GL_GOOGLE_include_directive : require
#include "quad_common.glsl.inc"

layout(location = 0) out vec2 v_pixel;
layout(location = 1) flat out uint v_index;

const vec2 CORNERS[6] = vec2[](
    vec2(0, 0), vec2(1, 0), vec2(1, 1),
    vec2(0, 0), vec2(1, 1), vec2(0, 1)
);

void main() {
    Quad q = quads[gl_InstanceIndex];
    vec4 r = q.rect;
    // Glyphs drawn at a fractional x offset spill one pixel to the right.
    if (uint(q.params.x) == KIND_GLYPH && q.params.y > 0.0) r.z += 1.0;

    // Shrink the quad to its clip rect so clipped geometry costs no fragments.
    vec2 lo = max(r.xy, q.clip.xy);
    vec2 hi = min(r.xy + r.zw, q.clip.zw);
    v_index = gl_InstanceIndex;
    if (any(lessThanEqual(hi, lo))) {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        v_pixel = vec2(0);
        return;
    }
    vec2 p = mix(lo, hi, CORNERS[gl_VertexIndex]);
    v_pixel = p;
    gl_Position = vec4(p / g.viewport * 2.0 - 1.0, 0.0, 1.0);
}
