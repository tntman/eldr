#version 450

// Lens distortion (see distortion.h): each output pixel takes the pinhole
// image's colour (bilinear), depth and ids (nearest) at Warp() of its
// screen point; nothing (the clear values) where that falls outside it.
layout(set = 0, binding = 0) uniform sampler2D color;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform isampler2D primId;
layout(set = 0, binding = 3) uniform isampler2D instanceId;

layout(push_constant) uniform Params {
    vec4 a;           // k1, k2, anaSq, scale
    vec4 b;           // center xy, asym xy
    vec4 c;           // tan of half the camera's horizontal view, overscan, aspect, fisheye
    vec4 clearColor;
    vec4 d;           // clear depth, p1, p2
    vec4 e;           // k3, k4, k5, k6
} pc;

layout(location = 0) out vec4 outColor;
layout(location = 1) out int outPrimId;
layout(location = 2) out int outInstanceId;

// As LensDistortion::Warp.
bool Warp(vec2 s, out vec2 source)
{
    vec2 q = s - pc.b.xy;
    q.x *= pc.a.z;
    float r2 = dot(q, q);
    bool valid = true;
    if (pc.c.w > 0.5) {
        // Equidistant fisheye: the angle off the axis grows with the
        // distance from the centre (in focal lengths); the pinhole image
        // has that angle at tan(angle) focal lengths.
        float rho = sqrt(r2) * pc.c.x, rho2 = rho * rho;
        float theta = rho * (1.0 + pc.a.x * rho2 + pc.a.y * rho2 * rho2);
        valid = theta < 1.5707;
        q *= rho > 1e-6 ? tan(min(theta, 1.5707)) / rho : 1.0;
    } else {
        float r4 = r2 * r2, r6 = r4 * r2;
        q *= (1.0 + pc.a.x * r2 + pc.a.y * r4 + pc.e.x * r6) / (1.0 + pc.e.y * r2 + pc.e.z * r4 + pc.e.w * r6);
    }
    // Tangential (OpenCV p1, p2), of the point before the radial terms.
    vec2 t = s - pc.b.xy;
    t.x *= pc.a.z;
    q += vec2(2.0 * pc.d.y * t.x * t.y + pc.d.z * (r2 + 2.0 * t.x * t.x),
              pc.d.y * (r2 + 2.0 * t.y * t.y) + 2.0 * pc.d.z * t.x * t.y);
    q += pc.b.zw * r2;
    q.x /= pc.a.z;
    source = pc.b.xy + q / pc.a.w;
    return valid;
}

void main()
{
    vec2 size = vec2(textureSize(color, 0));
    float aspect = pc.c.z;
    vec2 s = (gl_FragCoord.xy / size * 2.0 - 1.0) * vec2(1.0, 1.0 / aspect);
    vec2 source;
    bool valid = Warp(s, source);
    vec2 uv = source / pc.c.y * vec2(1.0, aspect) * 0.5 + 0.5;
    if (!valid || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        outColor = pc.clearColor;
        outPrimId = outInstanceId = -1;
        gl_FragDepth = pc.d.x;
        return;
    }
    ivec2 texel = min(ivec2(uv * size), ivec2(size) - 1);
    outColor = textureLod(color, uv, 0.0);
    gl_FragDepth = texelFetch(depth, texel, 0).r;
    outPrimId = texelFetch(primId, texel, 0).r;
    outInstanceId = texelFetch(instanceId, texel, 0).r;
}
