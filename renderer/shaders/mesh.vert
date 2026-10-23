#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

// Shared by the main pass (camera viewProj, culled instance lists) and the
// shadow pass (cascade viewProj, identity lists); the shadow pipeline has no
// fragment shader. gl_InstanceIndex includes the command's firstInstance,
// so it indexes the visible list directly.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inVertexColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    uint listBase;  // which list of instance indices (kVisibleLists); kFreeSlot: all instances
    uint motion;    // main pass: 1 = write outPrevWorldPos, 2 = same without last frame's joints
} pc;

// Last frame's joint matrices (copied before skin.comp rebuilds them) and
// rows of rigid meshes that moved: where each vertex was, for motion vectors.
layout(std430, set = 0, binding = 26) readonly buffer PrevJoints { mat4 prevJoints[]; };
layout(std430, set = 0, binding = 27) readonly buffer Motion { vec4 motionRows[]; };

// Crowds are bound by what the vertex stage exports, so the outputs are
// packed and the depth passes (built with DEPTH_ONLY) export only what
// mask.frag's alpha test reads. The w components carry last frame's world
// position (motion vectors).
#ifndef DEPTH_ONLY
layout(location = 0) out vec4 outWorldPos;  // xyz; w: last frame's x
layout(location = 1) out vec4 outNormal;    // xyz; w: last frame's y
layout(location = 3) out vec4 outColor;     // rgb; w: last frame's z
layout(location = 4) flat out uint outSlot;  // instance slot; mesh.frag reads the rest from it
#else
layout(location = 4) flat out uint outMaterial;
#endif
layout(location = 2) out vec2 outUv;

mat4 SkinMatrix(uvec4 s, vec4 w, uint base, bool previous)
{
    mat4 m0 = previous ? prevJoints[base + (s.x & 0xFFFFu)] : joints[base + (s.x & 0xFFFFu)];
    mat4 m1 = previous ? prevJoints[base + (s.x >> 16)] : joints[base + (s.x >> 16)];
    mat4 m2 = previous ? prevJoints[base + (s.y & 0xFFFFu)] : joints[base + (s.y & 0xFFFFu)];
    mat4 m3 = previous ? prevJoints[base + (s.y >> 16)] : joints[base + (s.y >> 16)];
    return (w.x * m0 + w.y * m1 + w.z * m2 + w.w * m3) / max(w.x + w.y + w.z + w.w, 1e-6);
}

// The depth prepass and the main pass must produce bit-identical depth.
invariant gl_Position;

void main()
{
    uint slot = pc.listBase == kFreeSlot ? uint(gl_InstanceIndex) : visible[pc.listBase + gl_InstanceIndex];
    Instance inst = instances[slot];
    DrawInfo d = draws[inst.draw];
    mat4 model = InstanceTransform(inst);
    mat3 normalMatrix;
    vec3 position = inPosition;
    mat4 prevModel = model;
#ifndef DEPTH_ONLY
    if (pc.motion != 0u && d.skin == kFreeSlot && d.motion != kFreeSlot) {
        uint row = (d.motion + uint(inst.index)) * 3u;
        prevModel = transpose(mat4(motionRows[row], motionRows[row + 1u], motionRows[row + 2u], vec4(0, 0, 0, 1)));
    }
#endif
    if (d.skin != kFreeSlot) {
        SkinVertex v = skin[d.skin + uint(gl_VertexIndex) - d.vertexBase];
        // Blend shapes move the rest point first (UsdSkel's order).
        if (d.blendShapes > 0u) {
            uint weights = d.blendWeights + (d.blendPerInstance != 0u ? uint(inst.index) * d.blendShapes : 0u);
            for (uint k = 0u; k < v.blend.y; ++k) {
                vec4 o = blendOffsets[v.blend.x + k];
                position += o.xyz * blendWeights[weights + min(uint(o.w), d.blendShapes - 1u)];
            }
        }
        // Linear blend skinning: the weighted sum of this vertex's joint matrices.
        uvec4 s = v.influence;
        vec4 w = vec4(unpackUnorm2x16(s.z), unpackUnorm2x16(s.w));
        model = SkinMatrix(s, w, inst.joints, false);
#ifndef DEPTH_ONLY
        // ponytail: last frame's blend shape weights aren't kept.
        prevModel = pc.motion == 1u ? SkinMatrix(s, w, inst.joints, true) : model;
#endif
        normalMatrix = mat3(model);  // ponytail: no inverse-transpose for scaled joints
    } else {
        normalMatrix = transpose(inverse(mat3(model)));
    }
    vec4 world = model * vec4(position, 1.0);
    gl_Position = pc.viewProj * world;
    // Hydra hands us GL clip space (z in [-w, w]); Vulkan clips z to [0, w].
    gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);
    outUv = inUv;
#ifndef DEPTH_ONLY
    vec3 prevWorld = pc.motion != 0u ? (prevModel * vec4(position, 1.0)).xyz : world.xyz;
    outWorldPos = vec4(world.xyz, prevWorld.x);
    outNormal = vec4(normalMatrix * inNormal, prevWorld.y);
    outColor = vec4(d.vertexColors != 0u ? inVertexColor.rgb : d.color.rgb, prevWorld.z);
    outSlot = slot;
#else
    outMaterial = d.material;
#endif
}
