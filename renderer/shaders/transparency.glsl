// Translucent surfaces in mesh.frag (renderer/transparency.h); include
// after mesh.frag's outputs.

// The pass a shading pipeline draws (the Renderer specializes it): 0 the
// opaque pass or sorted translucency (colour premultiplied by coverage,
// blended over), 1 weighted blended OIT accumulation.
layout(constant_id = 12) const uint kTranslucentPass = 0u;

// OIT: premultiplied colour and coverage times a weight falling with the
// distance (relative to the nearest translucent mesh's, to the power
// frame.translucency.w) are summed in location 0; location 3, the
// revealage, is multiplied by 1 - coverage. Pure highlights (glass at
// opacity 0) keep a sliver of coverage so they still count.
void TranslucentOutput(vec3 color, float alpha)
{
    if (kTranslucentPass != 1u) {
        return;
    }
    float a = clamp(alpha, 1.0 / 256.0, 1.0);
    float z = max(length(inWorldPos - frame.cameraPos.xyz) * frame.translucency.z, 1e-4);
    float w = a * clamp(pow(z, -frame.translucency.w), 1e-2, 1e3);
    outColor = vec4(color, a) * w;
    outVelocity = vec2(a);
}
