# Eldr vs EEVEE and Unreal

**Yes** = usable in production, **Partial** = real limits (see note), **No** = missing. EEVEE is Blender 5.2; its and Unreal's columns come from their docs and are indicative. *(interactive)* marks rows that matter mostly for games or live work.

## Rendering core

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Native USD / Hydra render delegate | Yes | No | Partial | Unreal imports USD stages; Blender only has a separate Hydra Storm add-on. |
| Converging final frames (sample accumulation) | Yes | Yes | Yes | Eldr: film. Unreal: Movie Render Queue. |
| Per-effect low-discrepancy sampling | Yes | Yes | Yes | Eldr: Sobol and blue-noise sample dimensions. |
| Adaptive sampling | Partial | No | No | Eldr stops when 99.9% of pixels converged; each sample still renders the whole frame. Not with motion blur time samples. |
| Path-traced reference mode | No | Yes (Cycles) | Yes | Eldr: use Karma in Houdini for ground truth. |
| Background shader compile and pipeline cache | Yes | Yes | Yes | |

## Lights and shadows

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Sphere, rect, disk, cylinder, distant and dome lights | Yes | Yes | Yes | |
| Textured area lights | Yes | No | Yes | Eldr: rect and disk lights, oriented per UsdLux. |
| IES profiles | No | No | Yes | Planned. |
| Light linking | No | Yes | Partial | Unreal has lighting channels. Planned for Eldr. |
| Light functions / gobos | No | No | Yes | |
| Light shapes visible to the camera | Yes | Yes | Yes | Eldr: `eldr:lightShapesVisible`; Karma's `renderlightgeo` hides single lights. |
| Soft shadows that converge exactly | Yes | Yes | Yes | Eldr jitters the light shape per film sample in final quality. |
| Virtual shadow maps | No | Yes | Yes | Eldr uses cascades and an atlas; ray-traced shadows cover final frames. |
| Ray-traced shadows | Yes | No | Yes | Eldr: a final-quality mode (`eldr:finalShadows`) or always (`eldr:rtShadows`); sun plus 15 lights, one ray per light and film sample, exact penumbra. |
| Ray-traced shadows through glass and volumes | Yes | No | Partial | Eldr: tinted by glass, marched through volumes (`eldr:rtTranslucentShadows`). |
| Coloured shadows through glass | Yes | Partial | Yes | Eldr: shadow maps and ray-traced shadows alike. |
| Volumes casting shadows | Yes | Yes | Yes | Eldr: sun and local lights. |
| Volumes shadowing each other | Yes | Yes | Partial | Eldr: sun, local lights and the dome's light from its pole (`eldr:volumeMutualShadows`). EEVEE: one shared froxel grid. |
| Sun extracted from an HDRI dome | Yes | Yes | No | |

## Global illumination and reflections

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Dynamic probe GI | Yes | Partial | Yes | Eldr: ray-traced DDGI probes with relocation, plus a per-pixel final gather in final quality. EEVEE: baked irradiance volumes. Unreal: Lumen. |
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
| Refraction through several layers, back faces, TIR | Yes | Partial | Yes | Screen space interactively; traced in final quality. |
| Ray-traced refraction (off-screen content) | Yes | No | Partial | Eldr: final quality (`eldr:rtRefraction`); glass behind glass, materials evaluated at every hit. |
| Ray-traced refraction through displaced glass | Yes | No | No | Eldr: rippled water bends the rays (`eldr:rtRefractionDisplacement`). |
| Holdouts and mattes | Yes | Yes | Yes | |
| OpenVDB / NanoVDB volumes | Yes | Yes | Yes | Eldr: `UsdVolVolume` with MaterialX or `kma_volume` materials, coloured extinction. Not Karma's Pyro Shader graph. |
| Houdini SOP volumes (no files) | Yes | No | No | Eldr: VDB and Houdini volumes from a SOP Import LOP, in the Houdini build. |
| Froxel fog | Yes | Yes | Yes | |
| Physical sky and atmosphere | No | Partial | Yes | EEVEE has a sky texture. |
| Volumetric clouds | No | No | Yes | |

## Materials and geometry

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| MaterialX, OpenPBR, standard_surface | Yes | No | Partial | Blender uses its own node system; Unreal imports MaterialX into Substrate. |
| GGX multiple-scattering energy compensation | Yes | Yes | Yes | Eldr: `eldr:multiScatter` (on) for standard_surface and OpenPBR; UsdPreviewSurface is single-scatter, as Karma's. |
| Textured lobe inputs (coat, film, transmission, SSS) | Yes | Yes | Yes | |
| Subsurface scattering | Yes | Yes | Yes | |
| Hair rendered as strands | Partial | Yes | Yes | Eldr: tubes or camera-facing ribbons with a hair BSDF. |
| Displacement, including from graphs | Yes | Yes | Yes | Unreal: Nanite tessellation. |
| Displacement seen by ray tracing, incl. per-instance graphs | Yes | No | Partial | Eldr: BLASes built from displaced vertices; graphs that differ per instance get one per instance (the 256 largest on screen). |
| Subdivision surfaces | Yes | Yes | Partial | Eldr: uniform levels, per mesh or from the host; no adaptive dicing. |
| Per-object ray visibility (camera, shadow, reflections) | Partial | Yes | Yes | Eldr: Karma's render visibility; reflections and GI only when ray traced, screen-space effects see what the camera sees. |
| LOD with crossfade | Yes | No | Yes | Unreal: Nanite. |
| Virtual geometry (Nanite-style) | No | No | Yes | *(interactive)* |
| Texture streaming | No | No | Yes | Needed for very large assets. |
| Ptex | No | No | No | Not planned. |

## Camera and motion

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Depth of field from lens samples | Yes | Yes | Yes | Eldr adds a residual post blur at low sample counts. |
| Motion blur from shutter time samples | Yes | Yes | Yes | Eldr: smooth blur between time samples (`eldr:motionBlurResidual`); not adaptive. |
| Deformation blur (point caches, skinning, blend shapes) | Yes | Yes | Partial | Eldr: skinned joints and blend shapes blur in time samples, from the shutter's samples in a single frame (sampled motion). |
| Motion blur on a single frame | Yes | Yes | Yes | Eldr: Hydra transform and point samples. |
| OpenCV lens distortion | Yes | No | Partial | |
| Upscaling while interactive | Partial | No | Yes | Eldr has its own TAA upscaler; Unreal has TSR and DLSS. *(interactive)* |

## Output and pipeline

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Multi-layer EXR with AOVs | Yes | Yes | Yes | |
| Light-split AOVs that sum to the beauty | Yes | Yes | Partial | Eldr: Karma's LPE names. |
| Cryptomatte with soft edges (3 layers, 6 ranks) | Yes | Partial | No | Eldr: coverage from the film's samples, Karma's `<var>00..02` layout. EEVEE: hard edges. |
| Cryptomatte | Yes | Yes | Yes | |
| UsdRenderSettings and render products | Yes | No | No | Eldr honours resolution, pixel aspect and data window. |
| Houdini Solaris | Partial | No | No | Eldr: Houdini 22 build renders in hython and husk (husk needs Indie or above); the viewport is untested. |
| usdview integration | Yes | No | No | |
