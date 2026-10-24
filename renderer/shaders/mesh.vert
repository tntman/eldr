#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "scene.glsl"
#include "displace.glsl"

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
    uint motion;    // main pass: 1 = write outPrevWorldPos, 2 = same without last frame's joints;
                    // + 4: with last frame's blend shape weights
} pc;

// Last frame's joint matrices (copied before skin.comp rebuilds them) and
// rows of rigid meshes that moved: where each vertex was, for motion vectors.
layout(std430, set = 0, binding = 26) readonly buffer PrevJoints { mat4 prevJoints[]; };
layout(std430, set = 0, binding = 27) readonly buffer Motion { vec4 motionRows[]; };
// Last frame's blend shape weights (copied after each frame's draws).
layout(std430, set = 0, binding = 60) readonly buffer PrevBlendWeights { float prevBlendWeights[]; };

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
#if defined(GRAPH) && !defined(DEPTH_ONLY)
// Material programs (built with GRAPH): vertex-rate primvars, packed in one
// vec4 per vertex.
layout(std430, set = 0, binding = 32) readonly buffer PrimvarVertices { vec4 primvarVertices[]; };
layout(location = 5) out vec4 outPrimvars;
#endif

mat4 Joint(uint i, bool previous)
{
    return previous ? prevJoints[i] : joints[i];
}

// Meshes with eight joint influences (two SkinVertex each, the second
// holding influences 4..7) are drawn by pipelines with this set; without it
// the shader is exactly the four-influence one (any extra code, even never
// run, cost a crowd's vertex stage ~4%).
layout(constant_id = 0) const bool kWideSkin = false;

// Dual-quaternion skinning (UsdSkel's skinningMethod "dualQuaternion"), in
// pipelines of its own like kWideSkin: each joint matrix splits into a
// rotation, a translation and the scale/shear left over; rotations and
// translations blend as dual quaternions (twisted joints keep their
// volume), the scale linearly, as UsdSkel's CPU skinning does.
// ponytail: the split is of the whole joint matrix (geomBind and the
// instance transform included), exact while those and the joints carry no
// non-uniform scale; culling bounds are the joints' linear hull, which
// dual quaternions can bulge past at strongly bent joints.
layout(constant_id = 1) const bool kDualQuaternion = false;

// A joint's rotation (Gram-Schmidt on its columns) as a quaternion (xyz, w),
// and the scale/shear it leaves: m = rotation * scale.
vec4 Rotation(mat3 m, out mat3 scale)
{
    vec3 x = normalize(m[0]);
    vec3 y = normalize(m[1] - dot(m[1], x) * x);
    mat3 r = mat3(x, y, cross(x, y));
    scale = transpose(r) * m;
    // Shepperd: from the largest of w, x, y, z (r[column][row]).
    float t = r[0][0] + r[1][1] + r[2][2];
    if (t > 0.0) {
        float s = 0.5 / sqrt(t + 1.0);
        return vec4((r[1][2] - r[2][1]) * s, (r[2][0] - r[0][2]) * s, (r[0][1] - r[1][0]) * s, 0.25 / s);
    }
    if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float s = 0.5 / sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]);
        return vec4(0.25 / s, (r[1][0] + r[0][1]) * s, (r[2][0] + r[0][2]) * s, (r[1][2] - r[2][1]) * s);
    }
    if (r[1][1] > r[2][2]) {
        float s = 0.5 / sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]);
        return vec4((r[1][0] + r[0][1]) * s, 0.25 / s, (r[2][1] + r[1][2]) * s, (r[2][0] - r[0][2]) * s);
    }
    float s = 0.5 / sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]);
    return vec4((r[2][0] + r[0][2]) * s, (r[2][1] + r[1][2]) * s, 0.25 / s, (r[0][1] - r[1][0]) * s);
}

mat3 RotationMatrix(vec4 q)
{
    vec3 q2 = 2.0 * q.xyz;
    float xx = q.x * q2.x, yy = q.y * q2.y, zz = q.z * q2.z;
    float xy = q.x * q2.y, xz = q.x * q2.z, yz = q.y * q2.z;
    float wx = q.w * q2.x, wy = q.w * q2.y, wz = q.w * q2.z;
    return mat3(1.0 - yy - zz, xy + wz, xz - wy, xy - wz, 1.0 - xx - zz, yz + wx, xz + wy, yz - wx, 1.0 - xx - yy);
}

// The dual-quaternion blend of the vertex's joints as a matrix: their
// rotations and translations flipped onto the first (strongest)
// influence's hemisphere, summed, normalized; then the blended scale.
mat4 DualQuaternionSkin(uvec4 s, vec4 w, uvec4 s2, uint base, bool previous)
{
    uint j[8] = uint[8](s.x & 0xFFFFu, s.x >> 16, s.y & 0xFFFFu, s.y >> 16,
                        s2.x & 0xFFFFu, s2.x >> 16, s2.y & 0xFFFFu, s2.y >> 16);
    vec4 w2 = kWideSkin ? vec4(unpackUnorm2x16(s2.z), unpackUnorm2x16(s2.w)) : vec4(0.0);
    float weight[8] = float[8](w.x, w.y, w.z, w.w, w2.x, w2.y, w2.z, w2.w);
    vec4 real = vec4(0.0), dual = vec4(0.0), pivot = vec4(0.0);
    mat3 scale = mat3(0.0);
    float sum = 0.0;
    for (int k = 0; k < (kWideSkin ? 8 : 4); ++k) {
        mat4 m = Joint(base + j[k], previous);
        mat3 sk;
        vec4 q = Rotation(mat3(m), sk);
        pivot = k == 0 ? q : pivot;
        float wq = dot(q, pivot) < 0.0 ? -weight[k] : weight[k];
        vec3 t = m[3].xyz;
        real += wq * q;
        dual += wq * 0.5 * vec4(q.w * t + cross(t, q.xyz), -dot(t, q.xyz));  // (t, 0) * q / 2
        scale += weight[k] * sk;
        sum += weight[k];
    }
    float len = max(length(real), 1e-6);
    real /= len;
    dual /= len;
    vec3 t = 2.0 * (real.w * dual.xyz - dual.w * real.xyz + cross(real.xyz, dual.xyz));  // 2 dual * conj(real)
    mat4 m = mat4(RotationMatrix(real) * (scale / max(sum, 1e-6)));
    m[3] = vec4(t, 1.0);
    return m;
}

// Linear blend skinning: the weighted sum of the vertex's joint matrices
// (16-bit joints in s.xy, unorm16 weights w; s2: influences 4..7).
mat4 SkinMatrix(uvec4 s, vec4 w, uvec4 s2, uint base, bool previous)
{
    if (kDualQuaternion) {
        return DualQuaternionSkin(s, w, s2, base, previous);
    }
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
    // Displaced along the rest normal before anything moves it, the same in
    // every pass (displace.glsl).
    vec3 position = Displace(inPosition, inNormal, d.material, inUv), prevPosition = position;
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
                uint weight = weights + min(uint(o.w), d.blendShapes - 1u);
                position += o.xyz * blendWeights[weight];
#ifndef DEPTH_ONLY
                prevPosition += o.xyz * ((pc.motion & 4u) != 0u ? prevBlendWeights[weight] : blendWeights[weight]);
#endif
            }
        }
        model = SkinMatrix(s, w, s2, inst.joints, false);
#ifndef DEPTH_ONLY
        prevModel = (pc.motion & 3u) == 1u ? SkinMatrix(s, w, s2, inst.joints, true) : model;
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
    vec3 prevWorld = pc.motion != 0u ? (prevModel * vec4(prevPosition, 1.0)).xyz : world.xyz;
    outWorldPos = vec4(world.xyz, prevWorld.x);
    outNormal = vec4(normalMatrix * inNormal, prevWorld.y);
    outColor = vec4(d.vertexColors != 0u ? inVertexColor.rgb : d.color.rgb, prevWorld.z);
    outSlot = slot;
#ifdef GRAPH
    outPrimvars = d.primvarVertices == kFreeSlot ? vec4(0.0)
                                                 : primvarVertices[d.primvarVertices + uint(gl_VertexIndex) - d.vertexBase];
#endif
#else
    outMaterial = d.material;
#endif
}
