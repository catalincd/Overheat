#version 450

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_local_pos;
layout(location = 2) in vec2 frag_size;
layout(location = 3) in float frag_corner_radius;
layout(location = 4) in float frag_border_width;
layout(location = 5) in vec4 frag_border_color;

layout(location = 0) out vec4 out_color;

float rounded_box_sdf(vec2 center_dist, vec2 half_size, float radius) {
    return length(max(abs(center_dist) - half_size + radius, 0.0)) - radius;
}

void main() {
    vec2 half_size = frag_size * 0.5;
    vec2 center_dist = frag_local_pos - half_size;
    float dist = rounded_box_sdf(center_dist, half_size, frag_corner_radius);
    float aa = fwidth(dist);

    float fill_alpha = 1.0 - smoothstep(-aa, aa, dist);
    if (fill_alpha < 0.001) discard;

    if (frag_border_width > 0.0) {
        float inner_dist = rounded_box_sdf(center_dist, half_size - frag_border_width, max(frag_corner_radius - frag_border_width, 0.0));
        float border_alpha = 1.0 - smoothstep(-aa, aa, inner_dist);
        vec4 color = mix(frag_color, frag_border_color, border_alpha * (1.0 - smoothstep(-aa, aa, dist + frag_border_width)));
        // Inside border = fill, on border = border color
        float inner_fill = smoothstep(-aa, aa, inner_dist);
        color = mix(frag_border_color, frag_color, 1.0 - inner_fill);
        out_color = vec4(color.rgb, color.a * fill_alpha);
    } else {
        out_color = vec4(frag_color.rgb, frag_color.a * fill_alpha);
    }
}
