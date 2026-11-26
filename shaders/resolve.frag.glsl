#version 450
// Translucent-window resolve: the offscreen image holds the frame as straight
// colour (rgb, the same composite the opaque path draws) plus window alpha (a).
// Compositors blend sRGB-encoded premultiplied pixels, so premultiply after
// encoding: stored = srgb(c) * a. An sRGB swapchain re-encodes what we write,
// hence the round trip through to_linear.
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(push_constant) uniform Resolve {
    uint premultiplied;  // 1: PRE_MULTIPLIED composite alpha, 0: POST_MULTIPLIED
    uint srgb_target;    // 1: swapchain format is *_SRGB
} pc;
layout(location = 0) out vec4 out_color;

vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
vec3 to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    vec4 s = texelFetch(frame, ivec2(gl_FragCoord.xy), 0);  // rgb decoded to linear
    vec3 enc = to_srgb(s.rgb);
    if (pc.premultiplied != 0u) enc *= s.a;
    out_color = vec4(pc.srgb_target != 0u ? to_linear(enc) : enc, s.a);
}
