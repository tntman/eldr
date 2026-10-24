#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "material.glsl"

// Alpha test of cutout materials in the depth-only passes (prepass,
// shadows). Opaque draws run those passes without a fragment shader.
// mesh.vert built with DEPTH_ONLY.
layout(location = 2) in vec2 inUv;
layout(location = 4) flat in uint inMaterial;
layout(location = 5) flat in uint inEntry;

// Also the depth prepass of meshes with levels of detail while they may
// crossfade (frame.lodFade): the fading copies' dither (frame.glsl).
void main()
{
    if ((inMaterial != kNoMaterial && !Present(materials[inMaterial], inUv)) || !LodKeep(inEntry, gl_FragCoord.xy)) {
        discard;
    }
}
