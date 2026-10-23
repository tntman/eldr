// Shading beyond mesh.frag's metallic-roughness base (Lambert diffuse, GGX
// specular), from the material's constants (material.glsl): the specular
// workflow. mesh.frag includes it after its inputs and calls PrepareLayers
// once per pixel, before the lights. Each costs a branch unless a material
// uses it and its FrameData.features.x flag is on.

// frame.features.x bits (renderer.cpp's Frame switches).
const uint kFeatureSpecularWorkflow = 8u;

// Before the lights: the base's F0 and colours as the material's lobes
// change them.
void PrepareLayers(inout vec3 f0)
{
    if (inMaterial == kNoMaterial) {
        return;
    }
    // UsdPreviewSurface's specular workflow: F0 is specularColor (F90
    // white); hydra/material.cpp already dropped metallic.
    vec4 specularColor = materials[inMaterial].specularColor;
    if (specularColor.w > 0.0 && (frame.features.x & kFeatureSpecularWorkflow) != 0u) {
        f0 = specularColor.rgb;
    }
}
