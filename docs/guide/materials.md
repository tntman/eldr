# Materials

Eldr renders the materials Houdini makes for Karma: MaterialX (standard_surface, OpenPBR, glTF PBR, Disney principled, unlit), Houdini's Material Library and Karma-context networks, and UsdPreviewSurface. You don't need special Eldr materials. Against Karma, Houdini's 121 basic and quick materials render with a median colour difference of 1.7 dE (barely visible).

> **Where in Houdini:** **Material Library LOP** with a **Karma Material Builder** or MaterialX nodes inside, then **Assign Material LOP**. The Material Palette's Karma materials work as they are.

![Houdini: Material Library LOP with a Karma Material Builder network](../images/houdini/material_library.png)

## What is supported

| Material | Support |
|---|---|
| MaterialX standard_surface | Every lobe: base, specular, metal, coat, sheen, thin film, anisotropy, transmission, subsurface, emission, opacity, normal maps, displacement |
| OpenPBR Surface | The same lobes under OpenPBR's names (fuzz, coat, thin film, transmission, subsurface) |
| UsdPreviewSurface | Metallic and specular workflows, clearcoat, ior, opacity and cutouts, displacement, normal maps |
| glTF PBR, Disney principled, surface_unlit | Their lobes mapped to the ones above |
| Karma's own nodes | Karma Ramp Const, Hex-Tiled Texture, Voronoi Noise 3D, Camera Falloff |
| Node graphs | Compiled whole on the GPU: maths, mixes, ramps, noises, textures, compositing, conditionals (**MaterialX Graphs**, on) |
| Volumes | MaterialX `volume` and Karma's `kma_volume`: see [Volumes](volumes.md) |

The [MaterialX coverage page](../materialx.md) has the measured node list.

![MaterialX graphs compiled on the GPU: compositing, glTF, Disney and Karma nodes (29_16, graphsOn)](../images/mtlx_graphs_on.jpg)
![MaterialX Graphs off: each material keeps its constant inputs (29_16, graphsOff)](../images/mtlx_graphs_off.jpg)

> **Tip:** If a node isn't supported, Eldr bypasses it (as MaterialX does) and counts it. The count shows as an orange **material fallback** warning in Houdini's render stats and in usdview's **Eldr > Render Statistics**.

> **Showcase:** open `showcase/2026-10-29_16_materialx_nodes/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `graphsOn` vs `graphsOff` (`eldr:materialGraphs` off), `proceduralsOff` (`eldr:proceduralTextures` off).

> **Showcase:** open `showcase/2026-10-29_01_opacity_mode/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `presence` vs `opacityModeOff` (both quads shaded as transparent).

## Material lobes

Each lobe has its own switch on the Eldr tab (**Advanced: Materials**), so you can see what it adds. Plain materials don't pay for lobes they don't use.

![Clearcoat, sheen, fuzz, thin film, anisotropy and hair (30_22, lobes)](../images/lobes.jpg)
![The lobes off: coats, sheen, iridescence and stretched highlights gone (30_22, lobesOff)](../images/lobes_off.jpg)

There is also a toon mode, for stylised looks: **Toon Bands** (or `inputs:eldr:toonBands` on one material).

![Toon Bands 3: every material in three flat bands (30_22, toon)](../images/toon.jpg)

> **Showcase:** open `showcase/2026-10-30_22_material_lobes/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `lobes` vs `lobesOff`, `toon` (`eldr:toonBands` 3).

## Subsurface scattering

Skin, wax, jade and leaves: light spreads under the surface and shines through thin parts. Eldr blurs the diffuse light in screen space, per colour channel (red goes furthest). **Subsurface Quality** (or Karma's SSS Quality) adds taps for a smoother blur.

![Subsurface on: soft skin and jade, ears and leaf glowing against the sun (30_13, subsurface)](../images/sss_on.jpg)
![Subsurface Scattering off: hard diffuse, dark ears (30_13, subsurfaceOff)](../images/sss_off.jpg)

> **Showcase:** open `showcase/2026-10-30_13_subsurface/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `subsurface` vs `subsurfaceOff`, `subsurfaceQuality`.

## Displacement

UsdPreviewSurface and MaterialX displacement move the vertices, so shadows, depth and ray tracing all see the new shape. It works best on subdivision surfaces (see [Crowds and performance](performance.md#subdivision-and-dicing)).

![Displacement on: bumps rise and cast shadows (30_18, displacement)](../images/displacement_on.jpg)
![Displacement off: flat planes (30_18, displacementOff)](../images/displacement_off.jpg)

> **Tip:** If a displacement graph outputs more than -1 to 1, set `inputs:eldr:displacementBound` on the shader to its largest offset, so the object isn't culled too early.

> **Showcase:** open `showcase/2026-10-30_18_displacement/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `displacement` vs `displacementOff`, `displacementHalf`.

> **Showcase:** open `showcase/2026-10-29_21_displacement_bound/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `culled` (frustum culling on, the default) vs `noCulling` (`eldr:frustumCulling` off).

## Textures and colour spaces

Textures read through UsdUVTexture or MaterialX `image` and `tiledimage`, with UDIMs (`<UDIM>`), wrap modes and several uv sets. Colour-space tags (ACEScg, sRGB, Rec.2020, P3, ...) convert to Eldr's linear Rec.709. **Texture Anisotropic Filtering** (16) keeps far textures sharp.

![Anisotropic filtering 16: the far floor stays crisp (30_21, textures)](../images/textures.jpg)
![Anisotropic filtering 1: the far floor smears (30_21, aniso1)](../images/textures_aniso1.jpg)

**Texture Compression** (off) packs 8-bit textures into BC7 to save GPU memory on big scenes; Karma never compresses, so it stays off by default.

> **Showcase:** open `showcase/2026-10-30_21_textures_colour/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `textures` vs `aniso1`.

> **Showcase:** open `showcase/2026-10-29_19_texture_compression/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `compressed` vs `exact`.

## Transparency and glass

- **Cutouts** (opacity with a threshold, leaves, fences) are alpha tested in every pass, shadows too.
- **Translucent** surfaces (opacity below 1) blend in depth order.
- **Glass** (transmission or `ior`) refracts the scene behind it. By default this is screen space; **Ray-Traced Refraction** follows rays through the glass. See [Ray-traced effects](ray-tracing.md#refraction-and-glass).
- **Holdouts** (Karma's Holdout Mode on Render Geometry Settings) render black with zero alpha but keep shadows: see [AOVs and output](aovs.md#holdouts-and-shadow-mattes).

> **Showcase:** open `showcase/2026-10-29_09_transparency_order/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `default`, `sorted` (`eldr:oit` off), `oneLayer` (`eldr:refractionLayers` 1), `volumesOverGlass` (`eldr:volumeUnderTransparency` off), `final` (traced refraction).

## Limitations

- Pure BSDF graphs (a `surface` made of pbrlib BSDF nodes) render grey with a warning.
- Coat has no own normal map or anisotropy.
- Subsurface is a blur, not a random walk: it can't show light deep inside thick objects.
- Karma's Pyro Shader isn't read (volumes render with their density only).
- More on the [How Eldr works](../features.md#materials) page.
