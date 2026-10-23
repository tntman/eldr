// Bindings shared by every post pass (same set layout everywhere).
// Only compute passes may write buffers: fragment-stage writes would need
// the fragmentStoresAndAtomics feature.
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
layout(set = 0, binding = 4) uniform sampler2D sceneDepth;  // 1.0 where no geometry was drawn

// Meaning of a/b differs per pass; see each shader.
layout(push_constant) uniform PostParams {
    vec4 a;
    vec4 b;
} pc;

float Luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Pixels still at the cleared depth show the app's background colour,
// unless the dome is drawn there (`sky`).
bool IsBackground(ivec2 p, float sky)
{
    return sky < 0.5 && texelFetch(sceneDepth, p, 0).r >= 1.0;
}
