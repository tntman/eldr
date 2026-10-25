#version 450

// Temporal anti-aliasing resolve (Karis 2014): reproject the history with
// the camera motion, clip it to the current 3x3 neighbourhood in YCoCg
// (variance clipping, Salvi 2016) and blend. All filtering happens on
// Reinhard-compressed colour so very bright HDR samples neither dominate
// nor ring (Catmull-Rom's negative lobes) into dark halos. Colour is
// premultiplied by coverage; alpha is resolved alongside it.
// The camera's motion comes from reprojecting the depth, each object's own
// motion from its motion vectors (see mesh.frag).

layout(set = 0, binding = 0) uniform sampler2D current;     // jittered HDR frame
layout(set = 0, binding = 1) uniform sampler2D history;     // last resolve, linear filter
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;  // window depth [0, 1]
layout(set = 0, binding = 3) uniform sampler2D velocity;    // object motion in last frame's uv

layout(push_constant) uniform TaaParams {
    mat4 reprojection;  // current jittered NDC -> previous clip (row-vector order)
    vec4 params;        // blend weight of the current frame, 1/width, 1/height
#ifdef UPSCALE
    // Upscaling (TAAU): xy where this frame's samples are from their input
    // pixels' centres (input pixels), zw input size / output size.
    vec4 upscale;
#endif
} pc;

layout(location = 0) out vec4 outColor;

float Luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec4 Compress(vec4 c)
{
    return vec4(c.rgb / (1.0 + Luma(c.rgb)), c.a);
}

vec4 Expand(vec4 c)
{
    return vec4(c.rgb / max(1.0 - Luma(c.rgb), 1e-4), c.a);
}

vec4 ToYCoCg(vec4 c)
{
    return vec4(dot(c.rgb, vec3(0.25, 0.5, 0.25)), dot(c.rgb, vec3(0.5, 0.0, -0.5)),
                dot(c.rgb, vec3(-0.25, 0.5, -0.25)), c.a);
}

vec4 FromYCoCg(vec4 c)
{
    return vec4(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z, c.a);
}

// NaN/Inf (and negative colour) is cleaned where it enters, so one bad
// value can neither poison the history nor spread through its filter.
vec4 Clean(vec4 c)
{
    return any(isnan(c)) || any(isinf(c)) ? vec4(0.0) : max(c, vec4(0.0));
}

vec4 Current(ivec2 p)
{
    return Clean(texelFetch(current, p, 0));
}

vec4 HistoryTap(vec2 uv)
{
    return Compress(Clean(textureLod(history, uv, 0.0)));
}

// Catmull-Rom history fetch from 9 bilinear taps (Jimenez/Pettineo):
// sharper than one bilinear tap, which would blur over the frames.
vec4 SampleHistory(vec2 uv, vec2 size)
{
    vec2 pos = uv * size;
    vec2 t1 = floor(pos - 0.5) + 0.5;
    vec2 f = pos - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (t1 - 1.0) / size;
    vec2 t3 = (t1 + 2.0) / size;
    vec2 t12 = (t1 + w2 / w12) / size;
    vec4 c = vec4(0.0);
    c += HistoryTap(vec2(t0.x, t0.y)) * w0.x * w0.y;
    c += HistoryTap(vec2(t12.x, t0.y)) * w12.x * w0.y;
    c += HistoryTap(vec2(t3.x, t0.y)) * w3.x * w0.y;
    c += HistoryTap(vec2(t0.x, t12.y)) * w0.x * w12.y;
    c += HistoryTap(vec2(t12.x, t12.y)) * w12.x * w12.y;
    c += HistoryTap(vec2(t3.x, t12.y)) * w3.x * w12.y;
    c += HistoryTap(vec2(t0.x, t3.y)) * w0.x * w3.y;
    c += HistoryTap(vec2(t12.x, t3.y)) * w12.x * w3.y;
    c += HistoryTap(vec2(t3.x, t3.y)) * w3.x * w3.y;
    return max(c, vec4(0.0));
}

#ifndef UPSCALE
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 cur = Current(p);
    float alpha = pc.params.x;
    if (alpha >= 1.0) {
        outColor = cur;  // no usable history (first frame, resize, TAA off)
        return;
    }

    vec2 uv = gl_FragCoord.xy * pc.params.yz;
    float depth = texelFetch(sceneDepth, p, 0).r;
    vec4 prev = pc.reprojection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec2 prevUv = prev.xy / prev.w * 0.5 + 0.5 + texelFetch(velocity, p, 0).xy;
    if (prev.w <= 0.0 || any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0)))) {
        outColor = cur;  // came from off-screen
        return;
    }

    ivec2 maxP = textureSize(current, 0) - 1;
    vec4 m1 = vec4(0.0), m2 = vec4(0.0);
    float brightest = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 c = Compress(Current(clamp(p + ivec2(x, y), ivec2(0), maxP)));
            brightest = max(brightest, Luma(c.rgb));
            c = ToYCoCg(c);
            m1 += c;
            m2 += c * c;
        }
    }
    vec4 mean = m1 / 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec4(0.0)));
    vec4 hist = ToYCoCg(SampleHistory(prevUv, 1.0 / pc.params.yz));
    hist = FromYCoCg(clamp(hist, mean - 1.25 * sigma, mean + 1.25 * sigma));

    // The clip box (and Catmull-Rom's overshoot while the camera moves) can
    // leave the history brighter than any pixel around it; near compressed
    // luma 1 Expand would multiply it by up to 10^4 and the next frame
    // would keep it. No brighter than the brightest neighbour, then.
    vec4 c = max(mix(hist, Compress(cur), alpha), vec4(0.0));
    float luma = Luma(c.rgb);
    c.rgb *= luma > brightest ? brightest / luma : 1.0;
    outColor = Expand(c);
}
#else
// Upscaling (TAAU): this frame has fewer samples than the output has
// pixels. Each output pixel takes the 3x3 input samples around it, each
// weighted by its distance from the pixel's centre (where its jitter put
// it, in output pixels; Karis 2014's Gaussian fit exp(-2.29 d^2)), and
// blends them with the history by how much weight landed: pixels no
// sample came near this frame keep their (clipped) history.
void main()
{
    vec2 inPos = gl_FragCoord.xy * pc.upscale.zw;  // the output pixel's centre, in input pixels
    ivec2 maxP = textureSize(current, 0) - 1;
    ivec2 q0 = ivec2(floor(inPos - pc.upscale.xy));  // the input sample nearest it
    vec4 sum = vec4(0.0), m1 = vec4(0.0), m2 = vec4(0.0);
    float weight = 0.0, brightest = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 q = q0 + ivec2(x, y);
            vec4 c = Compress(Current(clamp(q, ivec2(0), maxP)));
            vec2 d = (vec2(q) + 0.5 + pc.upscale.xy - inPos) / pc.upscale.zw;
            float w = exp(-2.29 * dot(d, d));
            sum += c * w;
            weight += w;
            brightest = max(brightest, Luma(c.rgb));
            c = ToYCoCg(c);
            m1 += c;
            m2 += c * c;
        }
    }
    vec4 cur = sum / max(weight, 1e-6);  // compressed
    float alpha = pc.params.x;
    ivec2 p = clamp(ivec2(inPos), ivec2(0), maxP);
    vec2 uv = gl_FragCoord.xy * pc.params.yz;
    float depth = texelFetch(sceneDepth, p, 0).r;
    vec4 prev = pc.reprojection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec2 prevUv = prev.xy / prev.w * 0.5 + 0.5 + texelFetch(velocity, p, 0).xy;
    if (alpha >= 1.0 || prev.w <= 0.0 || any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0)))) {
        outColor = Expand(cur);  // no usable history: the samples' reconstruction alone
        return;
    }
    vec4 mean = m1 / 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec4(0.0)));
    vec4 hist = ToYCoCg(SampleHistory(prevUv, 1.0 / pc.params.yz));
    hist = FromYCoCg(clamp(hist, mean - 1.25 * sigma, mean + 1.25 * sigma));
    vec4 c = max(mix(hist, cur, min(alpha * weight, 1.0)), vec4(0.0));
    float luma = Luma(c.rgb);
    c.rgb *= luma > brightest ? brightest / luma : 1.0;
    outColor = Expand(c);
}
#endif
