#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) out vec4 outColor;

void main()
{
    // Row 0 is NDC y = -1 (GL/Hydra bottom row), so no flip here.
    vec2 ndc = gl_FragCoord.xy / frame.viewport.xy * 2.0 - 1.0;
    vec4 nearP = frame.invViewProj * vec4(ndc, -1.0, 1.0);
    vec4 farP = frame.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);  // works for ortho too
    vec3 local = normalize(mat3(frame.envWorldToLocal) * dir);
    vec3 c = textureLod(textures[nonuniformEXT(frame.counts.w)], LatlongUv(local), 0.0).rgb;
    outColor = vec4(c * frame.envRadiance.rgb, 1.0);
}
