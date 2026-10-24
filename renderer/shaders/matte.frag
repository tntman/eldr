#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "material.glsl"

// Translucent holdouts (Renderer's matte pass, after the translucent ones):
// their opacity, blended as colour and alpha times 1 - opacity, cuts what
// they cover out of the image by how opaque they are. mesh.vert built
// with DEPTH_ONLY.
// ponytail: cuts out what's in front of the holdout too, and reads
// opacity from constants and textures only (no material programs).
layout(location = 2) in vec2 inUv;
layout(location = 4) flat in uint inMaterial;

layout(location = 0) out vec4 outMatte;

void main()
{
    float opacity = 1.0;
    if (inMaterial != kNoMaterial) {
        Material m = materials[inMaterial];
        opacity = clamp(Fetch(m.tex[6], m.params.wwww, inUv).r, 0.0, 1.0);
    }
    outMatte = vec4(0.0, 0.0, 0.0, opacity);
}
