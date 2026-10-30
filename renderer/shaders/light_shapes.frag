#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

// Light shapes seen by the camera (Frame::lightShapesVisible): the nearest
// sphere, rect, disk or cylinder light along each pixel's ray, in front of
// the opaque depth, replaces the colour with its emitted radiance. Lights
// whose color.w is even (hidden from the camera: Light::cameraVisible) and
// point or line lights are skipped. A rect or disk shows its texture
// (inputs:texture:file, with Frame::lightTextures), mapped as LightTint maps it.
// ponytail: no cone or focus shaping, no fog or translucency in front of
// it; rects and disks show from their front only.

#include "frame.glsl"
#include "lights.glsl"

layout(set = 0, binding = 63) uniform sampler2D sceneDepth;  // window [0, 1]

layout(push_constant) uniform Params {
    uint first;  // lights[first .. first + count)
    uint count;
} params;

layout(location = 0) out vec4 outColor;

vec3 Unproject(vec3 ndc)
{
    vec4 p = frame.invViewProj * vec4(ndc, 1.0);
    return p.xyz / p.w;
}

// Nearest hit of the ray (o, d) with the light's shape in front of t0, or t0.
float Hit(Light light, vec3 o, vec3 d, float t0)
{
    uint type = uint(light.position.w);
    vec3 c = light.position.xyz;
    if (type == kLightSphere) {
        if (light.halfWidth.w <= 0.0) {
            return t0;
        }
        vec3 oc = o - c;
        float b = dot(oc, d), q = dot(oc, oc) - light.axis.w * light.axis.w;
        float h = b * b - q;
        float t = -b - sqrt(max(h, 0.0));
        return h > 0.0 && t > 0.0 ? min(t, t0) : t0;
    }
    if (type == kLightCylinder) {
        float r = light.axis.w, len = length(light.halfWidth.xyz);
        if (light.halfWidth.w <= 0.0 || len <= 0.0) {
            return t0;
        }
        vec3 a = light.halfWidth.xyz / len, oc = o - c;
        vec3 dp = d - a * dot(d, a), op = oc - a * dot(oc, a);
        float qa = dot(dp, dp), qb = dot(op, dp), qc = dot(op, op) - r * r;
        float h = qb * qb - qa * qc;
        if (qa <= 1e-12 || h <= 0.0) {
            return t0;
        }
        float t = (-qb - sqrt(h)) / qa;
        return t > 0.0 && abs(dot(oc + d * t, a)) <= len ? min(t, t0) : t0;
    }
    // Rect and disk: the plane through the centre, seen from the emitting side.
    vec3 hw = light.halfWidth.xyz, hh = light.halfHeight.xyz, n = light.axis.xyz;
    float dn = dot(d, n);
    if (dn >= 0.0) {
        return t0;
    }
    float t = dot(c - o, n) / dn;
    vec3 p = o + d * t - c;
    vec2 uv = vec2(dot(p, hw) / max(dot(hw, hw), 1e-20), dot(p, hh) / max(dot(hh, hh), 1e-20));
    bool inside = type == kLightRect ? max(abs(uv.x), abs(uv.y)) <= 1.0 : dot(uv, uv) <= 1.0;
    return inside && t > 0.0 ? min(t, t0) : t0;
}

void main()
{
    vec2 ndc = gl_FragCoord.xy / frame.viewport.xy * 2.0 - 1.0;
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec3 o = Unproject(vec3(ndc, -1.0));
    vec3 d = normalize(Unproject(vec3(ndc, 1.0)) - o);
    float tScene = depth < 1.0 ? length(Unproject(vec3(ndc, depth * 2.0 - 1.0)) - o) : 3.0e38;
    float t = tScene;
    vec3 radiance = vec3(0.0);
    uint nearest = 0u;
    for (uint k = params.first; k < params.first + params.count; ++k) {
        Light light = lights[k];
        if ((uint(light.color.w) & 1u) == 0u) {
            continue;
        }
        float hit = Hit(light, o, d, t);
        if (hit < t) {
            t = hit;
            radiance = light.color.rgb;
            nearest = k;
        }
    }
    if (t >= tScene) {
        discard;
    }
    Light light = lights[nearest];
    uint type = uint(light.position.w);
    int tex = int(light.focusTint.w);
    if (tex >= 0 && (frame.reflections.z & kLightTextures) != 0u && (type == kLightRect || type == kLightDisk)) {
        vec3 hw = light.halfWidth.xyz, hh = light.halfHeight.xyz, p = o + d * t - light.position.xyz;
        vec2 uv = vec2(dot(p, hw) / dot(hw, hw), dot(p, hh) / dot(hh, hh));
        // Mip: one pixel's footprint on the light's plane, in texels.
        vec3 dx = normalize(Unproject(vec3(ndc.x + 2.0 / frame.viewport.x, ndc.y, 1.0)) - o);
        float footprint = t * length(dx - d) / max(abs(dot(d, light.axis.xyz)), 1e-3);
        ivec2 size = textureSize(textures[nonuniformEXT(tex)], 0);
        float lod = log2(max(footprint * float(max(size.x, size.y)) / (2.0 * min(length(hw), length(hh))), 1.0));
        radiance *= textureLod(textures[nonuniformEXT(tex)], vec2(0.5) + uv * vec2(-0.5, 0.5), lod).rgb;
    }
    outColor = vec4(radiance, 1.0);
}
