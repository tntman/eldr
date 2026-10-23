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
layout(location = 3) in vec4 inVertexColor;

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
    mat4 model = inst.transform;
    mat3 normalMatrix;
    if (d.skin != kFreeSlot) {
        // Linear blend skinning: the weighted sum of this vertex's joint matrices.
        uvec4 s = skin[d.skin + uint(gl_VertexIndex) - d.vertexBase];
        vec4 w = vec4(unpackUnorm2x16(s.z), unpackUnorm2x16(s.w));
        model = (w.x * joints[inst.joints + (s.x & 0xFFFFu)] + w.y * joints[inst.joints + (s.x >> 16)] +
                 w.z * joints[inst.joints + (s.y & 0xFFFFu)] + w.w * joints[inst.joints + (s.y >> 16)]) /
                max(w.x + w.y + w.z + w.w, 1e-6);
        normalMatrix = mat3(model);  // ponytail: no inverse-transpose for scaled joints
    } else {
        normalMatrix = transpose(inverse(mat3(model)));
    }
    vec4 world = model * vec4(inPosition, 1.0);
    gl_Position = pc.viewProj * world;
    // Hydra hands us GL clip space (z in [-w, w]); Vulkan clips z to [0, w].
    gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);
    outWorldPos = world.xyz;
    outNormal = normalMatrix * inNormal;
    outUv = inUv;
    outColor = d.vertexColors != 0u ? inVertexColor.rgb : d.color.rgb;
    outMaterial = d.material;
    outPrimId = d.primId;
    outInstanceId = inst.index;
}
