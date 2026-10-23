// std430 mirrors of Instance and DrawInfo in scene.h. Only the culling
// compute pass writes; vertex-stage writes would need an extra feature.
#ifdef SCENE_WRITABLE
#define SCENE_ACCESS
#else
#define SCENE_ACCESS readonly
#endif

const uint kFreeSlot = 0xFFFFFFFFu;

struct Instance {
    mat4 transform;  // USD row-major memory read column-major: model * p works
    uint draw;       // mesh slot, kFreeSlot if unused
    int index;       // index into the mesh's instances (instanceId AOV)
    uint joints;     // skinned: first joint matrix (transform is then culling bounds only)
    uint pad;
};

struct DrawInfo {
    vec4 boundsMin;  // object space
    vec4 boundsMax;
    vec4 color;      // displayColor
    uint material;
    int primId;
    uint visible;
    uint skin;        // first SkinVertex, kFreeSlot if rigid
    uint vertexBase;  // the mesh's first vertex in the vertex pool
    uint pad0, pad1, pad2;
};

layout(std430, set = 0, binding = 7) readonly buffer Instances { Instance instances[]; };
layout(std430, set = 0, binding = 8) readonly buffer Draws { DrawInfo draws[]; };
// [0, cap): visible instances per mesh (culling output), [cap, 2*cap): identity.
layout(std430, set = 0, binding = 9) SCENE_ACCESS buffer Visible { uint visible[]; };
// Skinning: per vertex four 16-bit joints (x, y) and unorm16 weights (z, w);
// joint matrices take rest points to world space.
layout(std430, set = 0, binding = 15) readonly buffer Skin { uvec4 skin[]; };
layout(std430, set = 0, binding = 16) readonly buffer Joints { mat4 joints[]; };
