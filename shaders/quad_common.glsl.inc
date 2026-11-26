// Shared between quad.vert.glsl and quad.frag.glsl.
#define KIND_RECT     0u
#define KIND_GLYPH    1u
#define KIND_SQUIGGLE 2u

struct Quad {
    vec4 rect;     // x, y, w, h (pixels)
    vec4 clip;     // x0, y0, x1, y1 (pixels)
    vec4 color;    // sRGB, straight alpha
    vec4 data;     // rect: radius, border_w | glyph: atlas x, y, w, h | squiggle: amplitude, wavelength, thickness
    vec4 color2;   // border color (sRGB)
    vec4 params;   // x: kind, y: subpixel x offset [0,1), z: fade_start x, w: fade_end x (off when z >= w)
};

layout(std140, set = 0, binding = 0) uniform Globals {
    vec2 viewport;
    float time;
    float text_gamma;
    float opacity;     // window background opacity (translucent mode only)
} g;

layout(std430, set = 0, binding = 1) readonly buffer Quads {
    Quad quads[];
};
