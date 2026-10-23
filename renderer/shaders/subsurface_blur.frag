#version 450

// Separable subsurface blur (renderer/subsurface.h). Pass 0 blurs the
// subsurface light horizontally; pass 1 blurs that vertically and outputs
// blurred minus unblurred light, added to the colour. Taps spread over
// three times the largest radius (at most 128 pixels), each weighted per
// channel by a Gaussian of that channel's radius over its distance in the
// world to the centre, so light doesn't cross depth jumps; taps of other
// materials (ids) are skipped.
// ponytail: a Gaussian per channel, not Burley's diffusion profile (which
// isn't separable); taps on a straight line only.

layout(set = 0, binding = 0) uniform sampler2D src;      // pass 0: the light, 1: after pass 0
layout(set = 0, binding = 1) uniform sampler2D light;    // rgb: subsurface light, a: largest radius
layout(set = 0, binding = 2) uniform isampler2D ids;     // radii's proportions (10 bits each), bit 30 set
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;

layout(push_constant) uniform BlurParams {
    mat4 invViewProj;
    vec4 params;  // direction (pixels), taps, pass
} pc;

layout(location = 0) out vec4 outColor;

vec3 World(ivec2 q, ivec2 size)
{
    float depth = texelFetch(sceneDepth, q, 0).r;
    vec4 p = pc.invViewProj * vec4((vec2(q) + 0.5) / vec2(size) * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(light, 0);
    int id = texelFetch(ids, q, 0).r;
    if (id < (1 << 30)) {
        discard;  // no subsurface material here
    }
    bool vertical = pc.params.w > 0.5;
    vec4 center = texelFetch(light, q, 0);
    vec3 radius = max(center.a * vec3(id & 1023, (id >> 10) & 1023, (id >> 20) & 1023) / 1023.0, vec3(1e-6));
    ivec2 dir = ivec2(pc.params.xy);
    vec3 p = World(q, size);
    float pixel = length(World(clamp(q + dir, ivec2(0), size - 1), size) - p);
    int taps = int(pc.params.z);
    float reach = min(3.0 * center.a / max(pixel, 1e-12), 128.0);  // pixels
    vec3 sum = texelFetch(src, q, 0).rgb;
    vec3 weights = vec3(1.0);
    if (reach >= 1.0 && taps > 1) {
        float spacing = reach / (0.5 * float(taps - 1));
        for (int i = 0; i < taps; ++i) {
            float o = (float(i) - 0.5 * float(taps - 1)) * spacing;
            ivec2 t = q + dir * int(round(o));
            if (t == q || any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size)) ||
                texelFetch(ids, t, 0).r != id) {
                continue;
            }
            float d = length(World(t, size) - p);
            vec3 w = exp(-0.5 * d * d / (radius * radius));
            sum += w * texelFetch(src, t, 0).rgb;
            weights += w;
        }
    }
    vec3 result = sum / weights;
    outColor = vertical ? vec4(result - center.rgb, 0.0) : vec4(result, 1.0);
}
