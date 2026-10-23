#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "fog.glsl"

layout(location = 0) out vec4 outColor;
layout(location = 1) out int outPrimId;      // the sky is not pickable
layout(location = 2) out int outInstanceId;
layout(location = 3) out vec2 outVelocity;  // the sky doesn't move by itself

// The dome (frame.counts.w: its latlong texture, kNone: none shown) behind
// the volumetric fog of the whole froxel range; premultiplied, so without
// a dome the fog alone covers the background.
void main()
{
    vec4 c = vec4(0.0);
    if (frame.counts.w != kNone) {
        // Row 0 is NDC y = -1 (GL/Hydra bottom row), so no flip here.
        vec2 ndc = gl_FragCoord.xy / frame.viewport.xy * 2.0 - 1.0;
        vec4 nearP = frame.invViewProj * vec4(ndc, -1.0, 1.0);
        vec4 farP = frame.invViewProj * vec4(ndc, 1.0, 1.0);
        vec3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);  // works for ortho too
        vec3 local = normalize(mat3(frame.envWorldToLocal) * dir);
        c = vec4(textureLod(textures[nonuniformEXT(frame.counts.w)], LatlongUv(local), 0.0).rgb * frame.envRadiance.rgb,
                 1.0);
    }
    vec4 fog = Fog(gl_FragCoord.xy, frame.fogAlbedo.w);
    outColor = vec4(c.rgb * fog.a + fog.rgb, 1.0 - (1.0 - c.a) * fog.a);
    outPrimId = -1;
    outInstanceId = -1;
    outVelocity = vec2(0.0);
}
