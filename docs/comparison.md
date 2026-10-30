# Eldr vs EEVEE and Unreal

**Yes** = usable in production, **Partial** = real limits (see note), **No** = missing. EEVEE is Blender 5.2; its and Unreal's columns come from their docs and are indicative. *(interactive)* marks rows that matter mostly for games or live work.

## Rendering core

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Native USD / Hydra render delegate | Yes | No | Partial | Unreal imports USD stages; Blender only has a separate Hydra Storm add-on. |
| Converging final frames (sample accumulation) | Yes | Yes | Yes | Eldr: film. Unreal: Movie Render Queue. |
| Per-effect low-discrepancy sampling | Yes | Yes | Yes | Eldr: Sobol and blue-noise sample dimensions. |
| Per-component sample quality (Karma's diffuse/reflect/light quality) | Yes | Partial | Yes | Eldr: `eldr:*Quality` or Karma's names, global (from the render settings), not per object. EEVEE: shadow rays and steps. Unreal: Lumen final gather and reflection quality. |
| Adaptive sampling | Yes | No | No | Eldr stops when 99.9% of pixels converged; 16x16 tiles that stopped skip shading and per-pixel rays (not under refraction, subsurface or post depth of field). Not with motion blur time samples. |
| Denoiser for the final image | Yes | Yes (OIDN, OptiX) | Partial | Eldr: Intel Open Image Denoise (OIDN) on the CPU (`eldr:denoise`), albedo and normal guides, once per converged film. Unreal: its path tracer only. |
| Path-traced reference mode | No | Yes (Cycles) | Yes | Eldr: use Karma in Houdini for ground truth. |
| Background shader compile and pipeline cache | Yes | Yes | Yes | |

## Lights and shadows

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Sphere, rect, disk, cylinder, distant and dome lights | Yes | Yes | Yes | |
| Textured area lights | Yes | No | Yes | Eldr: rect and disk lights, oriented per UsdLux. |
| IES profiles | Yes | No | Yes | Eldr: UsdLux `shaping:ies:*`, type C. |
| Light linking | Yes | Yes | Partial | Unreal has lighting channels. Eldr: UsdLux light and shadow links, 32 collections, not on domes. |
| Light functions / gobos | No | No | Yes | |
| Light shapes visible to the camera | Yes | Yes | Yes | Eldr: `eldr:lightShapesVisible`; Karma's `renderlightgeo` hides single lights. |
| Soft shadows that converge exactly | Yes | Yes | Yes | Eldr jitters the light shape per film sample (`eldr:shadowJitter`). |
| Virtual shadow maps | No | Yes | Yes | Eldr uses cascades and an atlas; ray-traced shadows cover final frames. |
| Ray-traced shadows | Yes | No | Yes | Eldr: `eldr:rtShadows` or `eldr:rtShadowsAuto`; sun plus up to 15 lights (`eldr:rtShadowLights`), one ray per light and film sample, exact penumbra. |
| Ray-traced shadows through glass and volumes | Yes | No | Partial | Eldr: tinted by glass, marched through volumes (`eldr:rtTranslucentShadows`). |
| Coloured shadows through glass | Yes | Partial | Yes | Eldr: shadow maps and ray-traced shadows alike. |
| Volumes casting shadows | Yes | Yes | Yes | Eldr: sun and local lights. |
| Volumes shadowing each other | Yes | Yes | Partial | Eldr: sun, local lights and the dome's light from its pole (`eldr:volumeMutualShadows`). EEVEE: one shared froxel grid. |
| Sun extracted from an HDRI dome | Yes | Yes | No | |

## Global illumination and reflections

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Dynamic probe GI | Yes | Partial | Yes | Eldr: ray-traced light probes (DDGI) that move off walls, plus a per-pixel final gather (`eldr:finalGather`). EEVEE: baked irradiance volumes. Unreal: Lumen. |
| Screen-space GI | Yes | Yes | Yes | Eldr: horizon or visibility-bitmask screen-space GI (SSGI). |
| GI at Lumen quality (surface cache, many bounces) | No | No | Yes | |
| Every diffuse bounce by default | Yes | Partial | Partial | Eldr's GI has no bounce limit. Karma's default `karma:object:diffuselimit` is 1, so a closed room renders darker in Karma (Cornell box: Eldr 1.27x Karma's default, within 3% of Karma at 10; docs/testing.md). |
| Screen-space reflections with a denoiser | Yes | Yes | Yes | |
| Ray-traced reflections | Yes | No | Yes | |
| Authored reflection probes | Yes | Yes | Yes | Eldr: up to 8, from flagged meshes. |
| Planar mirrors | Partial | Yes | Yes | Eldr: up to 4, blurred by roughness (`eldr:planarBlur`), no contact hardening. EEVEE's planars are glossy too. |

## Transparency, volumes and atmosphere

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Order-independent transparency | Yes | No | Partial | |
| Refraction through several layers, back faces, TIR | Yes | Partial | Yes | Screen space by default; traced with `eldr:rtRefraction`. |
| Ray-traced refraction (off-screen content) | Yes | No | Partial | Eldr: `eldr:rtRefraction`; glass behind glass, materials evaluated at every hit. |
| Ray-traced refraction through displaced glass | Yes | No | No | Eldr: rippled water bends the rays (`eldr:rtRefractionDisplacement`). |
| Holdouts and mattes | Yes | Yes | Yes | |
| OpenVDB / NanoVDB volumes | Yes | Yes | Yes | Eldr: `UsdVolVolume` with MaterialX or `kma_volume` materials, coloured extinction. Not Karma's Pyro Shader graph. |
| Houdini SOP volumes (no files) | Yes | No | No | Eldr: VDB and Houdini volumes from a SOP Import LOP, in the Houdini build. |
| Froxel fog (grid aligned to the camera view) | Yes | Yes | Yes | |
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
| Displacement seen by ray tracing, incl. per-instance graphs | Yes | No | Partial | Eldr: acceleration structures built from displaced vertices; graphs that differ per instance get one per instance (the 256 largest on screen). |
| Subdivision surfaces | Yes | Yes | Partial | Eldr: uniform levels per mesh, from the host, the mesh, or its size on screen (`eldr:screenDicing`) (Karma's dicing quality); no per-face adaptive dicing. |
| Per-object ray visibility (camera, shadow, reflections) | Partial | Yes | Yes | Eldr: Karma's render visibility; reflections and GI only when ray traced, screen-space effects see what the camera sees. |
| LOD with crossfade | Yes | No | Yes | Unreal: Nanite. |
| Virtual geometry (Nanite-style) | No | No | Yes | *(interactive)* |
| Texture streaming | No | No | Yes | Needed for very large assets. |
| Ptex | No | No | No | Not planned. |

## Camera and motion

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Depth of field from lens samples | Yes | Yes | Yes | Eldr adds a residual post blur at low sample counts. |
| Motion blur from shutter time samples | Yes | Yes | Yes | Eldr: smooth blur between time samples (`eldr:motionBlurResidual`); not adaptive. Traced and planar reflections blur too, probes don't. |
| Deformation blur (point caches, skinning, blend shapes) | Yes | Yes | Partial | Eldr: skinned joints and blend shapes blur in time samples, from the shutter's samples in a single frame (sampled motion). |
| Motion blur on a single frame | Yes | Yes | Yes | Eldr: Hydra transform and point samples. |
| OpenCV lens distortion | Yes | No | Partial | |
| Physical camera exposure (EV, ISO, shutter, f-stop) | Yes | Partial | Yes | Eldr: UsdGeomCamera's exposure attributes, as Karma (beauty and light AOVs, colour limit after exposure; Karma's ratios within 0.1% without the limit). EEVEE: one exposure value in colour management. |
| Upscaling while interactive | Partial | No | Yes | Eldr has its own temporal anti-aliasing (TAA) upscaler; Unreal has TSR and DLSS. *(interactive)* |

## Output and pipeline

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Multi-layer EXR with AOVs | Yes | Yes | Yes | |
| Light-split AOVs that sum to the beauty | Yes | Yes | Partial | Eldr: Karma's LPE names. |
| Per-AOV sample filter (min, max, closest, nearest, box, Gauss) | Yes | No | No | Eldr: Karma's Render Var "Sample Blending" or `driver:parameters:aov:eldr:filter`, film only. |
| Cryptomatte with soft edges (3 layers, 6 ranks) | Yes | Partial | No | Eldr: coverage from the film's samples, Karma's `<var>00..02` layout. EEVEE: hard edges. |
| Cryptomatte | Yes | Yes | Yes | |
| UsdRenderSettings and render products | Yes | No | No | Eldr honours resolution, pixel aspect and data window. |
| Houdini Solaris | Yes | No | No | Eldr: Houdini 22 build renders in the Solaris viewport, hython and husk (husk needs Indie or above), with its own Render Settings and Render Geometry Settings tabs. |
| usdview integration | Yes | No | No | |
