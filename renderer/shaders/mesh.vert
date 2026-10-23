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

mat4 Joint(uint i, bool previous)
{
    return previous ? prevJoints[i] : joints[i];
}

// Meshes with eight joint influences (two SkinVertex each, the second
// holding influences 4..7) are drawn by pipelines with this set; without it
// the shader is exactly the four-influence one (any extra code, even never
// run, cost a crowd's vertex stage ~4%).
layout(constant_id = 0) const bool kWideSkin = false;

// Linear blend skinning: the weighted sum of the vertex's joint matrices
// (16-bit joints in s.xy, unorm16 weights w; s2: influences 4..7).
mat4 SkinMatrix(uvec4 s, vec4 w, uvec4 s2, uint base, bool previous)
{
    mat4 m = w.x * Joint(base + (s.x & 0xFFFFu), previous) + w.y * Joint(base + (s.x >> 16), previous) +
             w.z * Joint(base + (s.y & 0xFFFFu), previous) + w.w * Joint(base + (s.y >> 16), previous);
    float sum = w.x + w.y + w.z + w.w;
    if (kWideSkin) {
        vec4 w2 = vec4(unpackUnorm2x16(s2.z), unpackUnorm2x16(s2.w));
        m += w2.x * Joint(base + (s2.x & 0xFFFFu), previous) + w2.y * Joint(base + (s2.x >> 16), previous) +
             w2.z * Joint(base + (s2.y & 0xFFFFu), previous) + w2.w * Joint(base + (s2.y >> 16), previous);
        sum += w2.x + w2.y + w2.z + w2.w;
    }
    return m / max(sum, 1e-6);
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
        uint sv = d.skin + (uint(gl_VertexIndex) - d.vertexBase) * (kWideSkin ? 2u : 1u);
        SkinVertex v = skin[sv];
        uvec4 s = v.influence, s2 = kWideSkin ? skin[sv + 1u].influence : uvec4(0u);
        vec4 w = vec4(unpackUnorm2x16(s.z), unpackUnorm2x16(s.w));
        // Blend shapes move the rest point first (UsdSkel's order).
        if (d.blendShapes > 0u) {
            uint weights = d.blendWeights + (d.blendPerInstance != 0u ? uint(inst.index) * d.blendShapes : 0u);
            for (uint k = 0u; k < v.blend.y; ++k) {
                vec4 o = blendOffsets[v.blend.x + k];
                position += o.xyz * blendWeights[weights + min(uint(o.w), d.blendShapes - 1u)];
            }
        }
        model = SkinMatrix(s, w, s2, inst.joints, false);
#ifndef DEPTH_ONLY
        // ponytail: last frame's blend shape weights aren't kept.
        prevModel = pc.motion == 1u ? SkinMatrix(s, w, s2, inst.joints, true) : model;
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
