// Volumetric fog (Renderer's Fog, fog.h); include after frame.glsl. The
// froxel grid is frame.fogGrid.xy screen tiles by fogGrid.z slices, slice
// k covering view depths D (k / Z)^2 .. D ((k + 1) / Z)^2 with D =
// frame.fogAlbedo.w. Texel k of the integrated volume holds the
// in-scattered light and transmittance from the camera to the far side of
// slice k.
layout(set = 0, binding = 48) uniform sampler3D fogVolume;

float FogSliceDepth(float t)
{
    return frame.fogAlbedo.w * t * t;
}

// (In-scattered radiance, transmittance) between the camera and the point
// at fragCoord (pixels) and view depth; nothing without fog.
vec4 Fog(vec2 fragCoord, float viewDepth)
{
    if (!Feature(kFeatureFog)) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    float slices = float(frame.fogGrid.z);
    float k = sqrt(clamp(viewDepth / frame.fogAlbedo.w, 0.0, 1.0)) * slices;  // slice coordinate
    vec4 fog = textureLod(fogVolume, vec3(fragCoord / frame.viewport.xy, (k - 0.5) / slices), 0.0);
    return k < 1.0 ? mix(vec4(0.0, 0.0, 0.0, 1.0), fog, k) : fog;  // fades in over the first slice
}
