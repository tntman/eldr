# Feature comparison: Eldr, EEVEE and Unreal

This page shows where Eldr stands against Blender's EEVEE (as of Blender 5.2)
and Unreal Engine 5. **Yes** means the feature is there and usable in
production. **Partial** means it exists with real limits, which the note
describes. **No** means it is missing.

Read it with these limits in mind:
- The EEVEE and Unreal columns come from their documentation and release notes
  as Claude knows them, plus the EEVEE study in
  [eevee-lardomar.md](eevee-lardomar.md). They are not checked against the
  latest builds, so treat single cells as indicative.
- Eldr's column is kept current as features are merged. Its "known
  limitations" are listed in [features.md](features.md#known-limitations).
- Eldr aims at final frames to EXR for VFX, not games. Rows that matter mostly
  for interactive or game work are marked *(interactive)*.

## Rendering core

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Native USD / Hydra render delegate | Yes | No | Partial | Unreal imports USD stages; Blender has a separate Hydra Storm add-on. |
| Converging final frames (sample accumulation) | Yes | Yes | Yes | Eldr's film, EEVEE's film, Unreal's Movie Render Queue. |
| Per-effect low-discrepancy sampling | Yes | Yes | Yes | Eldr: Sobol and blue-noise sample dimensions. |
| Adaptive sampling | Partial | No | No | Eldr stops when 99.9% of pixels converge; every sample still renders the whole frame. |
| Path-traced reference mode | No | Yes (Cycles) | Yes | Eldr relies on Karma in Houdini for ground truth. |
| Background shader compile and pipeline cache | Yes | Yes | Yes | |

## Lights and shadows

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Sphere, rect, disk, cylinder, distant and dome lights | Yes | Yes | Yes | |
| Textured area lights | Yes | No | Yes | Eldr: rect and disk lights, oriented per UsdLux. |
| IES profiles | No | No | Yes | Planned. |
| Light linking | No | Yes | Partial | Unreal has lighting channels. Planned for Eldr. |
| Light functions / gobos | No | No | Yes | |
| Light shapes visible to the camera | Yes | Yes | Yes | Eldr reads Karma's `renderlightgeo` per light. |
| Soft shadows that converge exactly | Yes | Yes | Yes | Eldr jitters the light shape per film sample in final quality. |
| Virtual shadow maps | No | Yes | Yes | Eldr uses cascades and an atlas; ray-traced shadows cover final frames. |
| Ray-traced shadows | Yes | No | Yes | Final quality: one ray per light and film sample, the sun plus 15 lights, through glass and volumes (`eldr:finalShadows`). |
| Coloured shadows through glass | Yes | Partial | Yes | With shadow maps and ray-traced shadows alike. |
| Volumes casting shadows | Yes | Yes | Yes | Eldr: sun and local lights. |
| Sun extracted from an HDRI dome | Yes | Yes | No | |

## Global illumination and reflections

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Dynamic probe GI | Yes | Partial | Yes | Eldr: ray-traced DDGI probes. EEVEE: baked irradiance volumes. Unreal: Lumen. |
| Screen-space GI | Yes | Yes | Yes | Eldr: horizon or visibility-bitmask SSGI. |
| GI at Lumen quality (surface cache, many bounces) | No | No | Yes | |
| Screen-space reflections with a denoiser | Yes | Yes | Yes | |
| Ray-traced reflections | Yes | No | Yes | |
| Authored reflection probes | Yes | Yes | Yes | Eldr: up to 8, from flagged meshes. |
| Planar mirrors | Partial | Yes | Yes | Eldr: up to 4, sharp only. EEVEE 5.1 added glossy planars. |

## Transparency, volumes and atmosphere

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Order-independent transparency | Yes | No | Partial | |
| Refraction through several layers, back faces, TIR | Yes | Partial | Yes | Screen space. |
| Ray-traced refraction (off-screen content) | No | No | Partial | In progress in round 4. |
| Holdouts and mattes | Yes | Yes | Yes | |
| OpenVDB / NanoVDB volumes | Yes | Yes | Yes | |
| Froxel fog | Yes | Yes | Yes | |
| Physical sky and atmosphere | No | Partial | Yes | EEVEE has a sky texture. |
| Volumetric clouds | No | No | Yes | |

## Materials and geometry

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| MaterialX, OpenPBR, standard_surface | Yes | No | Partial | Blender uses its own node system; Unreal imports MaterialX into Substrate. |
| Textured lobe inputs (coat, film, transmission, SSS) | Yes | Yes | Yes | |
| Subsurface scattering | Yes | Yes | Yes | |
| Hair rendered as strands | Partial | Yes | Yes | Eldr: tubes or camera-facing ribbons with a hair BSDF. |
| Displacement, including from graphs | Yes | Yes | Yes | Unreal: Nanite tessellation. |
| Subdivision surfaces | Yes | Yes | Partial | |
| LOD with crossfade | Yes | No | Yes | Unreal: Nanite. |
| Virtual geometry (Nanite-style) | No | No | Yes | *(interactive)* |
| Texture streaming | No | No | Yes | Needed for very large assets. |
| Ptex | No | No | No | Not planned. |

## Camera and motion

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Depth of field from lens samples | Yes | Yes | Yes | Eldr adds a residual post blur at low sample counts. |
| Motion blur from shutter time samples | Yes | Yes | Yes | |
| Deformation blur (point caches, skinning, blend shapes) | Yes | Yes | Partial | |
| Motion blur on a single frame | Yes | Yes | Yes | Eldr: Hydra transform and point samples. |
| OpenCV lens distortion | Yes | No | Partial | |
| Upscaling while interactive | Partial | No | Yes | Eldr has its own TAA upscaler; Unreal has TSR and DLSS. *(interactive)* |

## Output and pipeline

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Multi-layer EXR with AOVs | Yes | Yes | Yes | |
| Light-split AOVs that sum to the beauty | Yes | Yes | Partial | Eldr uses Karma's LPE names. |
| Cryptomatte | Yes | Yes | Yes | |
| UsdRenderSettings and render products | Yes | No | No | Eldr honours resolution, pixel aspect and data window. |
| Houdini Solaris | Partial | No | No | Eldr builds for Houdini 22 and renders in hython. The viewport is untested, and husk needs Houdini Indie or above. |
| usdview integration | Yes | No | No | |
