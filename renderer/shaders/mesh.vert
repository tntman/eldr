#version 450

// Shared by the main pass (camera viewProj) and the shadow pass (cascade
// viewProj); the shadow pipeline has no fragment shader.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
// Per-instance (firstInstance + instance index). USD matrices are row-major
// row-vector; loading rows as columns yields the transpose GLSL needs.
layout(location = 3) in vec4 inModel0;
layout(location = 4) in vec4 inModel1;
layout(location = 5) in vec4 inModel2;
layout(location = 6) in vec4 inModel3;
layout(location = 7) in vec3 inColor;
layout(location = 8) in uint inMaterial;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUv;
layout(location = 3) out vec3 outColor;
layout(location = 4) flat out uint outMaterial;

void main()
{
    mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);
    vec4 world = model * vec4(inPosition, 1.0);
    gl_Position = pc.viewProj * world;
    // Hydra hands us GL clip space (z in [-w, w]); Vulkan clips z to [0, w].
    gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);
    outWorldPos = world.xyz;
    outNormal = transpose(inverse(mat3(model))) * inNormal;
    outUv = inUv;
    outColor = inColor;
    outMaterial = inMaterial;
}
