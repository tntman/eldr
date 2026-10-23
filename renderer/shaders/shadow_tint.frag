#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "material.glsl"

// Translucent shadow casters into Transparency's tint maps: the colour
// they let through, which the colour pass multiplies together; the depth
// pass (colour masked) keeps the nearest that lets anything less than
// everything through. mesh.vert built with DEPTH_ONLY.
// ponytail: constants and textures only (material programs are skipped),
// no absorption over the path inside.
layout(location = 2) in vec2 inUv;
layout(location = 4) flat in uint inMaterial;

layout(location = 0) out vec4 outTint;

void main()
{
    vec3 t = vec3(1.0);
    if (inMaterial != kNoMaterial) {
        Material m = materials[inMaterial];
        float opacity = clamp(Fetch(m.tex[6], m.params.wwww, inUv).r, 0.0, 1.0);
        // What transmits through a refracting surface loses its two
        // interfaces' reflections at normal incidence.
        float f0 = (m.refraction.x - 1.0) / (m.refraction.x + 1.0);
        float interfaces = (1.0 - f0 * f0) * (1.0 - f0 * f0);
        vec3 transmitted = opacity * m.transmission.w * m.transmission.rgb * interfaces;
        t = m.refraction.w > 0.0 ? vec3((1.0 - opacity) * interfaces) : vec3(1.0 - opacity) + transmitted;
    }
    if (all(greaterThanEqual(t, vec3(0.999)))) {
        discard;
    }
    outTint = vec4(t, 1.0);
}
