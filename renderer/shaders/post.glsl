// Bindings shared by every post pass (same set layout everywhere).
// Only compute passes may write buffers: fragment-stage writes would need
// the fragmentStoresAndAtomics feature.
//
// The HDR input is premultiplied by coverage: the scene is cleared to
// transparent black and geometry and sky write alpha 1, so alpha < 1 means
// (partly) the app's background colour, composited in display space by the
// tonemap pass.
#ifdef POST_WRITABLE
#define POST_ACCESS
#else
#define POST_ACCESS readonly
#endif

layout(set = 0, binding = 0) uniform sampler2D src;       // HDR scene or a bloom mip
layout(set = 0, binding = 1) uniform sampler2D bloom;     // bloom mip 0
layout(std430, set = 0, binding = 2) POST_ACCESS buffer Histogram { uint bins[256]; };
layout(std430, set = 0, binding = 3) POST_ACCESS buffer Exposure {
    float ev;         // adapted exposure, log2 of the multiplier
    float targetEv;
    float avgLogLum;
    float pad;
} exposure;

// Meaning of a/b/c differs per pass; see each shader.
layout(push_constant) uniform PostParams {
    vec4 a;
    vec4 b;
    vec4 c;
} pc;

float Luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}
