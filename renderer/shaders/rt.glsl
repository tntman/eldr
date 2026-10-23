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
// (tlas.comp: 1 promoted to a light, 2 cutout).
struct RtSlot {
    uvec2 blas;
    uint firstIndex;
    uint flags;
};
layout(std430, set = 0, binding = 57) readonly buffer RtSlots { RtSlot rtSlots[]; };

// Instance masks: ordinary geometry, and emissive meshes promoted to
// lights (no shadow rays see them: they would shadow their own light).
const uint kMaskSurface = 1u, kMaskEmitter = 2u, kMaskAll = 0xFFu;

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
    float pad;
};
const uint kHitBackface = 1u;  // the shading normal faces away from the ray

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

vec3 RtColor(uint v)
{
    return vec3(unpackHalf2x16(floatBitsToUint(rtVertices[v * 10u + 8u])),
                unpackHalf2x16(floatBitsToUint(rtVertices[v * 10u + 9u])).x);
}
