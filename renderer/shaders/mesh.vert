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

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUv;
layout(location = 3) out vec3 outColor;
layout(location = 4) flat out uint outMaterial;
layout(location = 5) flat out int outPrimId;
layout(location = 6) flat out int outInstanceId;

// The depth prepass and the main pass must produce bit-identical depth.
invariant gl_Position;

void main()
{
    Instance inst = instances[visible[gl_InstanceIndex]];
    DrawInfo d = draws[inst.draw];
    vec4 world = inst.transform * vec4(inPosition, 1.0);
    gl_Position = pc.viewProj * world;
    // Hydra hands us GL clip space (z in [-w, w]); Vulkan clips z to [0, w].
    gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);
    outWorldPos = world.xyz;
    outNormal = transpose(inverse(mat3(inst.transform))) * inNormal;
    outUv = inUv;
    outColor = d.color.rgb;
    outMaterial = d.material;
    outPrimId = d.primId;
    outInstanceId = inst.index;
}
