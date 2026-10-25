#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "material.glsl"

// Translucent holdouts (Renderer's matte pass, after the translucent ones):
// their opacity, blended as colour and alpha times 1 - opacity, cuts what
// they cover out of the image by how opaque they are. mesh.vert built
// with DEPTH_ONLY, or, for materials with a program (GRAPH), the shading
// pass's, so the opacity comes from the program as in mesh.frag.
// ponytail: cuts out what's in front of the holdout too.
#ifdef GRAPH
#include "scene.glsl"
layout(location = 0) in vec4 inWorldPosPrevX;
layout(location = 1) in vec4 inNormalPrevY;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inColorPrevZ;
layout(location = 4) flat in uint inEntry;
layout(location = 5) in vec4 inPrimvars;
#define inSlot EntrySlot(inEntry)
#define inWorldPos (inWorldPosPrevX.xyz)
#define inNormal (inNormalPrevY.xyz)
#define inColor (inColorPrevZ.rgb)
#include "program.glsl"
#define inMaterial (draws[instances[inSlot].draw].material)
#else
layout(location = 2) in vec2 inUv;
layout(location = 4) flat in uint inMaterial;
#endif

layout(location = 0) out vec4 outMatte;

void main()
{
    float opacity = 1.0;
    if (inMaterial != kNoMaterial) {
        Material m = materials[inMaterial];
        opacity = clamp(Fetch(m.tex[6], m.params.wwww, inUv).r, 0.0, 1.0);
#ifdef GRAPH
        if (m.program.y > 0u && (frame.features.x & kFeatureMaterialGraphs) != 0u) {
            vec3 baseColor = inColor, emissive = vec3(0.0), tangentNormal = vec3(0.0, 0.0, 1.0);
            float metallic = 0.0, roughness = 0.5, occlusion = 1.0;
            bool hasNormalMap = false;
            RunProgram(m.program, baseColor, emissive, metallic, roughness, occlusion, tangentNormal, hasNormalMap,
                       opacity);
        }
#endif
    }
    outMatte = vec4(0.0, 0.0, 0.0, opacity);
}
