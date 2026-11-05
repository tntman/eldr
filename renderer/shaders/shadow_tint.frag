#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "material.glsl"
#include "scene.glsl"

// Translucent shadow casters into Transparency's tint maps: the colour
// they let through, which the colour pass multiplies together; the depth
// pass (colour masked) keeps the nearest that lets anything less than
// everything through. A caustic caster's glass lets nothing through: under
// caustics its light arrives as photons; without them it would gather into
// a caustic that isn't drawn, and its shadow stays dark around it, as
// Karma's does. ponytail: no focus without caustics. mesh.vert built with
// DEPTH_ONLY.
layout(location = 2) in vec2 inUv;
layout(location = 4) flat in uint inMaterial;
layout(location = 5) flat in uint inEntry;

layout(location = 0) out vec4 outTint;

void main()
{
    vec3 t = vec3(1.0);
    if (inMaterial != kNoMaterial) {
        t = ShadowTransmission(materials[inMaterial], inUv);
    }
    if ((draws[instances[EntrySlot(inEntry)].draw].caustics & 1u) != 0u) {
        t = vec3(0.0);  // DrawInfo::caustics' caster bit
    } else if (Feature(kFeatureTintCastersOnly)) {
        discard;  // Frame::translucentShadows off: the casters alone
    }
    if (all(greaterThanEqual(t, vec3(0.999)))) {
        discard;
    }
    outTint = vec4(t, 1.0);
}
