#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "material.glsl"

// Translucent shadow casters into Transparency's tint maps: the colour
// they let through, which the colour pass multiplies together; the depth
// pass (colour masked) keeps the nearest that lets anything less than
// everything through. mesh.vert built with DEPTH_ONLY.
layout(location = 2) in vec2 inUv;
layout(location = 4) flat in uint inMaterial;

layout(location = 0) out vec4 outTint;

void main()
{
    vec3 t = vec3(1.0);
    if (inMaterial != kNoMaterial) {
        t = ShadowTransmission(materials[inMaterial], inUv);
    }
    if (all(greaterThanEqual(t, vec3(0.999)))) {
        discard;
    }
    outTint = vec4(t, 1.0);
}
