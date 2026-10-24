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

#include "skinning.glsl"

// The depth prepass and the main pass must produce bit-identical depth.
invariant gl_Position;

// Camera-facing primitives (DrawInfo::facing, vkr::Mesh::facing), in the
// kDisplacement pipelines only: the world offset of this vertex from its
// curve or point (world `p`), turned towards the eye of this pass's
// viewProj (the camera's, or a light's in shadow passes), and the shading
// normal. Ribbons: inNormal is the tangent times the half width, inUv.y
// the side; their normals lean 45 degrees out at the edges, a rounded
// strand. Discs: inNormal the offset within the disc times the radius
// (zero at the centre, whose normal faces the eye, the rim's points out:
// a sphere's shading).
vec3 Facing(uint kind, vec3 p, mat3 m, out vec3 normal)
{
    // The eye: the point viewProj's rows x, y and w all vanish at (a
    // direction for orthographic views, then away from growing depth).
    mat4 rows = transpose(pc.viewProj);
    vec4 a = rows[0], b = rows[1], c = rows[3];
    vec4 eye = vec4(dot(a.yzw, cross(b.yzw, c.yzw)), -dot(a.xzw, cross(b.xzw, c.xzw)),
                    dot(a.xyw, cross(b.xyw, c.xyw)), -dot(a.xyz, cross(b.xyz, c.xyz)));
    vec3 e = abs(eye.w) > 1e-6 * length(eye.xyz) ? eye.xyz / eye.w - p
                                                  : eye.xyz * -sign(dot(eye.xyz, rows[2].xyz));
    e = normalize(e);
    float scale = pow(abs(determinant(m)), 1.0 / 3.0);
    if (kind == 1u) {
        vec3 t = m * inNormal;
        float halfWidth = length(inNormal) * scale;
        t = dot(t, t) > 1e-30 ? normalize(t) : vec3(0.0, 1.0, 0.0);
        vec3 side = cross(t, e);
        side = dot(side, side) > 1e-12 ? normalize(side) : normalize(cross(t, abs(t.x) < 0.9 ? vec3(1, 0, 0)
                                                                                          : vec3(0, 1, 0)));
        float s = inUv.y > 0.5 ? 1.0 : -1.0;
        normal = normalize(cross(side, t) + s * side);
        return s * halfWidth * side;
    }
    vec3 right = normalize(cross(b.xyz, e)), up = cross(e, right);
    vec3 offset = (inNormal.x * right + inNormal.y * up) * scale;
    normal = dot(inNormal, inNormal) > 0.0 ? normalize(offset) : e;
    return offset;
}

void main()
{
    uint slot = pc.listBase == kFreeSlot ? uint(gl_InstanceIndex) : visible[pc.listBase + gl_InstanceIndex];
    Instance inst = instances[slot];
    DrawInfo d = draws[inst.draw];
    mat4 model = InstanceTransform(inst);
    mat3 normalMatrix;
    // Displaced along the rest normal before anything moves it, the same in
    // every pass (displace.glsl).
    bool facing = kDisplacement && d.facing != 0u;
    vec3 position = facing ? inPosition : Displace(inPosition, inNormal, d.material, inUv), prevPosition = position;
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
    vec3 facingOffset = vec3(0.0), facingNormal = vec3(0.0);
    if (facing) {
        facingOffset = Facing(d.facing, world.xyz, mat3(model), facingNormal);
        world.xyz += facingOffset;
    }
    gl_Position = pc.viewProj * world;
    // Hydra hands us GL clip space (z in [-w, w]); Vulkan clips z to [0, w].
    gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);
    outUv = inUv;
#ifndef DEPTH_ONLY
    vec3 prevWorld = pc.motion != 0u ? (prevModel * vec4(prevPosition, 1.0)).xyz + facingOffset : world.xyz;
    outWorldPos = vec4(world.xyz, prevWorld.x);
    outNormal = vec4(facing ? facingNormal : normalMatrix * inNormal, prevWorld.y);
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
