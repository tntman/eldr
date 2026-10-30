#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "material.glsl"

// One of the camera's translucent layers (VolumePass::TranslucentLayers,
// Renderer::_RenderTranslucentDepth), mesh.vert built with DEPTH_ONLY:
// the nearest fragment in front of the opaque depth and behind the last
// layer (translucentDepth), with the colour it lets through (as
// shadow_tint.frag's) and its distance along the ray from the near plane
// (volume.frag's t).
layout(location = 2) in vec2 inUv;
layout(location = 4) flat in uint inMaterial;

layout(location = 0) out vec4 outLayer;

layout(set = 0, binding = 63) uniform sampler2D sceneDepth;        // window [0, 1]
layout(set = 0, binding = 68) uniform sampler2D translucentDepth;  // the last layer's

vec3 Unproject(vec3 ndc)
{
    vec4 p = frame.invViewProj * vec4(ndc, 1.0);
    return p.xyz / p.w;
}

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    if (gl_FragCoord.z >= texelFetch(sceneDepth, q, 0).r || gl_FragCoord.z <= texelFetch(translucentDepth, q, 0).r) {
        discard;
    }
    vec3 t = vec3(1.0);
    if (inMaterial != kNoMaterial) {
        t = ShadowTransmission(materials[inMaterial], inUv);
    }
    if (all(greaterThanEqual(t, vec3(0.999)))) {
        discard;  // lets everything through: not a layer
    }
    vec2 ndc = gl_FragCoord.xy / frame.viewport.xy * 2.0 - 1.0;
    outLayer = vec4(t, length(Unproject(vec3(ndc, gl_FragCoord.z * 2.0 - 1.0)) - Unproject(vec3(ndc, -1.0))));
}
