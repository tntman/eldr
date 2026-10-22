#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 cameraPos;
} pc;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 outColor;

void main()
{
    // Flat shading with a headlight; abs() makes winding irrelevant.
    vec3 n = normalize(cross(dFdx(inWorldPos), dFdy(inWorldPos)));
    vec3 v = normalize(pc.cameraPos.xyz - inWorldPos);
    outColor = vec4(inColor * (0.25 + 0.75 * abs(dot(n, v))), 1.0);
}
