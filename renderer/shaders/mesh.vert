#version 450

layout(location = 0) in vec3 inPosition;
// Per-instance (firstInstance = draw index). USD matrices are row-major
// row-vector; loading rows as columns yields the transpose GLSL needs.
layout(location = 1) in vec4 inModel0;
layout(location = 2) in vec4 inModel1;
layout(location = 3) in vec4 inModel2;
layout(location = 4) in vec4 inModel3;
layout(location = 5) in vec4 inColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 cameraPos;
} pc;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outColor;

void main()
{
    vec4 world = mat4(inModel0, inModel1, inModel2, inModel3) * vec4(inPosition, 1.0);
    gl_Position = pc.viewProj * world;
    // Hydra hands us GL clip space (z in [-w, w]); Vulkan clips z to [0, w].
    gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);
    outWorldPos = world.xyz;
    outColor = inColor.rgb;
}
