// Body of quad.frag.glsl and quad_mask.frag.glsl (included after #version).
#include "quad_common.glsl.inc"

layout(set = 0, binding = 2) uniform sampler2D atlas;

layout(location = 0) in vec2 v_pixel;
layout(location = 1) flat in uint v_index;
#ifdef OH_ALPHA_MASK
// Translucent-window variant (dual-source blending, see Renderer2D). Colour
// blends with out_cover.a exactly as in the opaque path, so the rgb channels
// hold the same straight composite; the alpha channel instead accumulates
// window opacity: fills count at g.opacity, text and squiggles at full.
layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_cover;
#else
layout(location = 0) out vec4 out_color;
#endif

vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

float rounded_box(vec2 p, vec2 half_size, float radius) {
    vec2 d = abs(p) - half_size + radius;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
}

// 8-sample rotated grid, only evaluated near the edge.
const vec2 SS[8] = vec2[](
    vec2(-0.375, -0.125), vec2(-0.125,  0.375), vec2( 0.125, -0.375), vec2( 0.375,  0.125),
    vec2(-0.4375, 0.3125), vec2(-0.0625, -0.3125), vec2( 0.3125, 0.4375), vec2( 0.0625, -0.0625)
);

float box_coverage(vec2 p, vec2 half_size, float radius) {
    float d = rounded_box(p, half_size, radius);
    if (d < -0.75) return 1.0;
    if (d > 0.75) return 0.0;
    float acc = 0.0;
    for (int i = 0; i < 8; i++) acc += step(rounded_box(p + SS[i], half_size, radius), 0.0);
    return acc / 8.0;
}

float texel(ivec2 origin, ivec2 size, ivec2 t) {
    if (t.x < 0 || t.y < 0 || t.x >= size.x || t.y >= size.y) return 0.0;
    return texelFetch(atlas, origin + t, 0).r;
}

float glyph_coverage(Quad q, vec2 local) {
    ivec2 origin = ivec2(q.data.xy);
    ivec2 size = ivec2(q.data.zw);
    ivec2 t = ivec2(floor(local));
    float c0 = texel(origin, size, t);
    float frac = q.params.y;
    if (frac <= 0.0) return c0;
    // Shift the bitmap right by `frac` pixels: blend with the left neighbour.
    return mix(c0, texel(origin, size, t - ivec2(1, 0)), frac);
}

// Signed distance (approximate: max over edges) to a convex quad; negative inside.
float poly_distance(vec2 p, vec2 v[4]) {
    float area = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 a = v[i], b = v[(i + 1) & 3];
        area += a.x * b.y - b.x * a.y;
    }
    float s = area >= 0.0 ? 1.0 : -1.0;
    float d = -1e9;
    for (int i = 0; i < 4; i++) {
        vec2 a = v[i], e = v[(i + 1) & 3] - a;
        float len = length(e);
        if (len < 1e-4) continue;
        d = max(d, -s * (e.x * (p.y - a.y) - e.y * (p.x - a.x)) / len);
    }
    return d;
}

void main() {
    Quad q = quads[v_index];
    if (v_pixel.x < q.clip.x || v_pixel.y < q.clip.y || v_pixel.x >= q.clip.z || v_pixel.y >= q.clip.w)
        discard;

    vec2 local = v_pixel - q.rect.xy;
    uint kind = uint(q.params.x);
    vec4 color = vec4(to_linear(q.color.rgb), q.color.a);
    float alpha = 1.0;

    if (kind == KIND_RECT) {
        vec2 half_size = q.rect.zw * 0.5;
        vec2 p = local - half_size;
        float radius = min(q.data.x, min(half_size.x, half_size.y));
        float border = q.data.y;
        alpha = box_coverage(p, half_size, radius);
        if (border > 0.0) {
            float inner = box_coverage(p, half_size - border, max(radius - border, 0.0));
            color = mix(vec4(to_linear(q.color2.rgb), q.color2.a), color, inner);
        }
    } else if (kind == KIND_GLYPH) {
        float cov = glyph_coverage(q, local);
        // Linear-space blending thins dark text; boost coverage less as text gets lighter.
        float lum = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
        alpha = pow(cov, mix(1.0 / g.text_gamma, 1.0, lum));
    } else if (kind == KIND_POLY) {
        vec2 v[4] = vec2[](q.data.xy, q.data.zw, q.color2.xy, q.color2.zw);
        float d = poly_distance(local, v);
        alpha = clamp(0.5 - d, 0.0, 1.0);
        if (q.params.y > 0.0) alpha *= clamp(0.5 + d + q.params.y, 0.0, 1.0);
    } else if (kind == KIND_SQUIGGLE) {
        float amp = q.data.x, k = 6.2831853 / q.data.y, thick = q.data.z;
        float wave = q.rect.w * 0.5 + amp * sin(local.x * k);
        float slope = amp * k * cos(local.x * k);
        float dist = abs(local.y - wave) / sqrt(1.0 + slope * slope);
        alpha = clamp(thick * 0.5 + 0.5 - dist, 0.0, 1.0);
    }

    if (q.params.z < q.params.w)
        alpha *= 1.0 - smoothstep(q.params.z, q.params.w, v_pixel.x);

    alpha *= color.a;
    if (alpha <= 0.002) discard;
#ifdef OH_ALPHA_MASK
    float target = kind == KIND_RECT ? g.opacity : 1.0;
    out_color = vec4(color.rgb * alpha, alpha * target);
    out_cover = vec4(alpha);
#else
    out_color = vec4(color.rgb * alpha, alpha);
#endif
}
