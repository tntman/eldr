// std430 mirrors of Instance and DrawInfo in scene.h. Only the culling
// compute pass writes; vertex-stage writes would need an extra feature.
#ifdef SCENE_WRITABLE
#define SCENE_ACCESS
#else
#define SCENE_ACCESS readonly
#endif
#ifdef JOINTS_WRITABLE
#define JOINTS_ACCESS
#else
#define JOINTS_ACCESS readonly
#endif

const uint kFreeSlot = 0xFFFFFFFFu;

struct Instance {
    vec4 rows[3];    // affine transform, rows of the column-vector form
    uint draw;       // mesh slot, kFreeSlot if unused
    int index;       // index into the mesh's instances (instanceId AOV)
    uint joints;     // skinned: first of the instance's joint matrices
    uint pad;
};

mat4 InstanceTransform(Instance inst)
{
    return transpose(mat4(inst.rows[0], inst.rows[1], inst.rows[2], vec4(0.0, 0.0, 0.0, 1.0)));
}

struct DrawInfo {
    vec4 boundsMin;  // object space
    vec4 boundsMax;  // w: the material's largest displacement (displace.glsl)
    vec4 color;      // displayColor
    uint material;
    int primId;
    uint visible;
    uint skin;        // first SkinVertex, kFreeSlot if rigid
    uint vertexBase;  // the mesh's first vertex in the vertex pool
    uint vertexColors;  // 1: per-vertex displayColor, else color
    uint jointCount;
    uint skinXforms;       // first skinning transform
    uint jointBounds;      // first JointBounds (two vec4 each)
    uint skinPerInstance;  // 1: jointCount skinning transforms per instance
    uint blendWeights;     // first blend shape weight
    uint blendShapes;      // weights per pose; 0: no blend shapes
    uint blendPerInstance; // 1: blendShapes weights per instance
    uint motion;           // first instance's rows in motionRows, kFreeSlot: didn't move
    uint primvars;         // its material program's primvar slots (program.glsl), kFreeSlot: none
    uint primvarVertices;  // its first vec4 of vertex-rate primvars, kFreeSlot: none
    uint hair;             // 1: shaded as hair (brdf.glsl)
    uint holdout;          // 1: a holdout (transparency.glsl)
};

// One per vertex, two with eight influences (mesh.vert's kWideSkin).
struct SkinVertex {
    uvec4 influence;  // four 16-bit joints (x, y), four unorm16 weights (z, w)
    uvec4 blend;      // first blend shape offset, count
};

layout(std430, set = 0, binding = 7) readonly buffer Instances { Instance instances[]; };
layout(std430, set = 0, binding = 8) readonly buffer Draws { DrawInfo draws[]; };
// kVisibleLists lists of instance indices, instanceCapacity each (culling output).
layout(std430, set = 0, binding = 9) SCENE_ACCESS buffer Visible { uint visible[]; };
// Skinning: per vertex its joint influences and blend shape range; joint
// matrices take rest points to world space (built by skin.comp from the
// skinning transforms and the instance transform); per joint the rest
// bounds of the vertices it moves (min, max); blend shape offsets (xyz,
// shape index) and weights.
layout(std430, set = 0, binding = 15) readonly buffer Skin { SkinVertex skin[]; };
layout(std430, set = 0, binding = 16) JOINTS_ACCESS buffer Joints { mat4 joints[]; };
layout(std430, set = 0, binding = 21) readonly buffer SkinXforms { mat4 skinXforms[]; };
layout(std430, set = 0, binding = 22) readonly buffer JointBounds { vec4 jointBounds[]; };
layout(std430, set = 0, binding = 23) readonly buffer BlendOffsets { vec4 blendOffsets[]; };
layout(std430, set = 0, binding = 24) readonly buffer BlendWeights { float blendWeights[]; };
