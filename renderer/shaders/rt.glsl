// Shared by the ray tracing stages (raytracing.cpp): the scene's TLAS and
// geometry, and what a traced ray returns. Include after frame.glsl and
// scene.glsl; define RAY_TRACING before material.glsl.

// The TLAS: one instance per instance slot, its custom index the mesh slot.
layout(set = 0, binding = 52) uniform accelerationStructureEXT tlas;
// The scene's pools: Vertex (10 floats: position, normal, uv, colour as
// four halves) and triangle indices (relative to DrawInfo::vertexBase).
layout(std430, set = 0, binding = 55) readonly buffer RtVertices { float rtVertices[]; };
layout(std430, set = 0, binding = 56) readonly buffer RtIndices { uint rtIndices[]; };
// Per mesh slot: its BLAS's address (0: none), first index, flags
// (kRtSlotEmitter, 2: cutout, kRtSlotTranslucent, see tlas.comp).
struct RtSlot {
    uvec2 blas;
    uint firstIndex;
    uint flags;
};
layout(std430, set = 0, binding = 57) readonly buffer RtSlots { RtSlot rtSlots[]; };

// Every instance of the mesh is also a light (Scene::SetEmissiveLights).
const uint kRtSlotEmitter = 1u;
// Alpha-tested: any-hit shaders run, else forced opaque (tlas.comp).
const uint kRtSlotCutout = 2u;
// A translucent mesh: only shadow rays that tint see it (rt_shadow.rahit).
const uint kRtSlotTranslucent = 4u;

// Skinned instances (rt_skin.comp): the TLAS's instances from
// rt.skinned.x on, one per entry here, each with a BLAS of its own over its
// posed vertices (world-space position and normal, 6 floats each).
struct RtSkinned {
    uvec2 blas;
    uint slot;         // mesh slot
    uint firstVertex;  // in rtSkinnedVertices
    uint instance;     // instance slot
    uint vertexCount;
    uint flags;        // 1: eight influences, 2: dual quaternions
    uint pad;
};
layout(std430, set = 1, binding = 5) readonly buffer RtSkinnedTable { RtSkinned rtSkinned[]; };
layout(std430, set = 1, binding = 4) readonly buffer RtSkinnedVertices { float rtSkinnedVertices[]; };

// Ray payload (location 0) of traced rays.
struct RtHit {
    vec3 position;  // world
    float t;        // in: the ray cone's spread angle (texture filtering); out: hit distance, < 0: missed
    vec3 normal;    // shading normal, facing the ray
    float roughness;
    vec3 geometric;  // geometric normal, facing the ray
    uint flags;      // kHit*
    vec3 baseColor;
    float metallic;
    vec3 emissive;
    uint slot;      // the hit's mesh slot
    vec2 bary;      // its barycentrics (rt_refraction.rgen evaluates its material there)
    uint triangle;
    uint instance;  // TLAS instance (gl_InstanceID)
};
const uint kHitBackface = 1u;  // the shading normal faces away from the ray
const uint kHitEmitter = 2u;   // a light stands in for its emission (kRtSlotEmitter)
const uint kHitTranslucent = 4u;  // a translucent mesh (kRtSlotTranslucent)

// A TLAS instance's light link mask (Instance::linkMask): its instance
// slot's, a skinned one's through rtSkinned. A macro: gi.glsl's rt comes
// after this file.
#define RtLinkMask(i)     (instances[(i) < rt.skinned.x ? (i) : rtSkinned[(i) - rt.skinned.x].instance].linkMask)

// Corner k (0..2) of a mesh slot's triangle in the vertex pool; then its
// position, normal, uv (object space) and displayColor.
uint RtVertex(uint slot, uint triangle, uint k)
{
    return draws[slot].vertexBase + rtIndices[rtSlots[slot].firstIndex + 3u * triangle + k];
}

vec3 RtPosition(uint v)
{
    return vec3(rtVertices[v * 10u], rtVertices[v * 10u + 1u], rtVertices[v * 10u + 2u]);
}

vec3 RtNormal(uint v)
{
    return vec3(rtVertices[v * 10u + 3u], rtVertices[v * 10u + 4u], rtVertices[v * 10u + 5u]);
}

vec2 RtUv(uint v)
{
    return vec2(rtVertices[v * 10u + 6u], rtVertices[v * 10u + 7u]);
}

vec3 RtSkinnedPosition(uint v)
{
    return vec3(rtSkinnedVertices[v * 6u], rtSkinnedVertices[v * 6u + 1u], rtSkinnedVertices[v * 6u + 2u]);
}

vec3 RtSkinnedNormal(uint v)
{
    return vec3(rtSkinnedVertices[v * 6u + 3u], rtSkinnedVertices[v * 6u + 4u], rtSkinnedVertices[v * 6u + 5u]);
}

vec3 RtColor(uint v)
{
    return vec3(unpackHalf2x16(floatBitsToUint(rtVertices[v * 10u + 8u])),
                unpackHalf2x16(floatBitsToUint(rtVertices[v * 10u + 9u])).x);
}
