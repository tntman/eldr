# How Eldr works

Eldr is a Hydra 2.0 render delegate on Vulkan 1.3 for OpenUSD 26.08. It is a rasterizer (clustered forward shading, GPU culling and skinning); ray tracing, where the GPU has it, adds shadows, reflections, refraction and GI. Every feature has an `eldr:` setting.

| Section | Topic |
|---|---|
| Quick recipes | Rendering a frame, final settings |
| Frame pipeline | GPU passes in order |
| Film | Sample accumulation |
| Lighting and shadows | Lights, shadow modes |
| Global illumination | Probes, SSGI, AO |
| Reflections | SSR, probes, planar, ray-traced |
| Transparency | OIT, refraction, holdouts |
| Volumes and fog | OpenVDB, froxel fog |
| Materials | MaterialX, OpenPBR, lobes |
| Geometry | Subdivision, displacement, LOD, skinning |
| Camera | DOF, motion blur, distortion |
| Output | AOVs, EXR, Cryptomatte, light splits |
| Performance | Pipelines, caches |
| USD authoring conventions | Primvars and attributes |
| Known limitations | |

## Quick recipes

Render a frame to EXR with a render settings prim (needs a USD built with OpenImageIO; Eldr writes the files once the frame converged):

```
usdenv.cmd usdrecord --renderer Eldr --camera /cam --renderSettingsPrimPath /Render/settings scene.usda out.png
```

Settings from Python:

```python
engine.SetRendererPlugin("HdVkRendererPlugin")
engine.SetRendererSetting("eldr:filmRestart", True)
engine.SetRendererSetting("eldr:filmSamples", 256)
```

| Goal | Settings |
|---|---|
| Final render | A settings prim with the final settings (docs/settings.md, "Final render"; `tests/scenes/final_settings.usda`), `eldr:filmSamples` 128-512 |
| Linear ungraded EXR for comp | Default (`eldr:productLinear`); `eldr:autoExposure` off and `eldr:exposureCompensation` 0 for unexposed values (the camera's exposure still applies, as in Karma; `eldr:cameraExposure` off ignores it) |
| True soft shadows, bokeh, motion blur | Leave `eldr:shadowJitter`, `eldr:dofJitter`, `eldr:motionBlurTimeSamples` on (defaults) |
| Indirect light | `eldr:gi`, `eldr:ssgi` (+ `eldr:ssgiBitmask` for thin occluders) |
| Accurate off-screen shadows and reflections | `eldr:rtShadows` (`eldr:rtShadowLights` 16, `eldr:rtShadowSamples` 1) or `eldr:rtShadowsAuto`; `eldr:rtReflections` with 1 ray |
| Clean geometry | `eldr:lodBias` -1 or `eldr:lod` off; `--complexity veryhigh` or `eldr:maxRefineLevel` |
| Fireflies | `eldr:directClamp`, `eldr:indirectClamp` |

| Environment variable | Effect |
|---|---|
| `HDVK_NO_RAY_TRACING=1` | Pretend the GPU has no ray tracing |
| `HDVK_NO_POSE_SHARE=1` | A/B: with deferred skinning each mesh poses its skeleton itself (default: once per skeleton and shutter time, shared by its meshes) |
| `HDVK_PIPELINE_CACHE_DIR` | Move the pipeline cache (default `%LOCALAPPDATA%\Eldr`) |
| `HDVK_RT_EARLY_COMPILE=0` | Compile the ray tracing pipelines on their first trace instead of starting the pixels and probes ones on threads at the delegate's start (A/B) |
| `USD_ROOT`, `USD_VENV`, `HDVK_BUILD` | Read by `usdenv.cmd` (sets plugin paths) |

## Frame pipeline

GPU passes in order. Names are the timers in usdview's **Eldr > Render Statistics** and `GetRenderStats()["gpuPassMs"]`. (opt) = runs only when needed.

| # | Pass | What it does |
|---|---|---|
| 0 | `probeCapture`, `planarReflection` (opt) | Draw the probe cube and mirrored views |
| 1 | `skinning` | Last frame's joints (sampled motion: the shutter's) kept for motion, then GPU joint matrices from skeletons |
| 2 | `shutter` (opt) | Moves instances, joints, blend shapes, camera to the sample's shutter time |
| 3 | `cullCamera`, `cullShadows` | Frustum and size culling, LOD choice, indirect draws: the camera's, then the 4 sun cascades' |
| 4 | `lightClusters` | Local lights into 16x9x24 view clusters |
| 5 | `sunCascade0`..`3`, `sunShadows`, `sunShadowMin` | The 4 cascades' depth, one each; translucent and volume tint maps; min-depth reduction for the soft search |
| 6 | `depthPrepass` | Depth plus cutout alpha tests |
| 7 | `accelerationStructures`, `rayTracing` (opt) | BLAS/TLAS; traced shadows, reflections, GI probe updates |
| 8 | `ssgi` or `ambientOcclusion` | SSGI (replaces AO) or GTAO |
| 9 | `localShadows` | Atlas for sphere, rect, disk, spot, cylinder lights |
| 10 | `fog` (opt) | Froxel fog |
| 11 | `shading` | Sky, then opaque PBR: sun, local lights, dome, probes, GI, SSR, fog. Writes colour, ids, motion, light splits |
| 12 | `ssrDenoise` (opt) | Glossy SSR filters |
| 13 | `subsurface` (opt) | Screen-space subsurface blur |
| 14 | `volumes` (opt) | OpenVDB march; `volumesInFront` is the part before the farthest glass layer (all of it under traced refraction) |
| 15 | `translucency` (opt) | Refraction layers, OIT, holdouts |
| 16 | `depthOfField` (opt) | Post DOF (skipped with lens jitter) |
| 17 | `film` or `taa` | Film average or TAA resolve |
| 18 | `motionBlur` (opt) | Post blur (skipped with time samples) |
| 19 | `post` | Auto exposure, bloom, tonemap |
| 20 | `aovs` (opt) | G-buffer and packing for bound AOVs |
| 21 | `lensDistortion` (opt) | Warps colour, depth, ids |
| 22 | `readback` | Colour and depth to OpenGL on the GPU, or to the CPU |

One frame in flight (synchronous). The 20k-agent crowd renders in about 21 ms at 1280x720 with defaults.

## Film

Three ways to turn jittered samples into a pixel.

| Mode | When | How |
|---|---|---|
| Interactive TAA | Camera or scene moves | Halton 2,3 jitter; Catmull-Rom history with YCoCg clipping and motion vectors. Effects loop over `eldr:interactiveSamples` (8) noise samples. `eldr:renderScale` < 1 upscales. |
| Film | Still for 3 frames; first frame of a fresh history (husk, usdrecord) | fp32 weighted average through a Gaussian filter (`eldr:filmFilterRadius`, 1.5 px) until `eldr:filmSamples` (64): then converged (`IsConverged`, `percentDone`). Any camera, scene or setting change restarts it; a camera prim synced again unchanged (as Houdini's free camera may be) does not. `eldr:taaSamples` only sets the jitter cycle. |
| Film restart | `eldr:filmRestart` | The film restarts from scratch at once after every change, never from the TAA image. |

There is no quality mode (lane 65): husk and usdrecord render with the same settings as a viewport. Offline (husk: `rasterRenderProducts`; `enableInteractive = false`, the hdPrman convention usdrecord follows; not in Houdini's viewport, marked by `houdini:viewport` or `houdini:interactive`) changes only speed, never the image: an execute renders film samples back to back for up to `eldr:offlineBatchSeconds` (1 s) and reads back only the last (viewports keep one sample per execute), the delegate tears down synchronously, no pipeline prewarm, every bound AOV renders, subdivision waits for its re-dice.

What the film enables (each has a switch, all on by default):

| Feature | Setting | Notes |
|---|---|---|
| Exact soft-shadow penumbrae | `eldr:shadowJitter` | Film samples only; TAA frames keep PCSS |
| True depth of field with occlusion | `eldr:dofJitter` | |
| Motion blur at shutter times | `eldr:motionBlurTimeSamples` | |
| Fog without history | `eldr:fogFilmJitter` | |
| GI preconvergence | `eldr:giPreconverge` | Film waits for fresh probes |
| Adaptive sampling | `eldr:adaptiveThreshold` (0 = off) | Converged once `eldr:adaptivePixelFraction` (0.999) of pixels stopped. 16x16 tiles whose pixels and neighbours stopped skip shading and per-pixel rays (`eldr:adaptiveTiles`, on; not with refraction, subsurface or post depth of field in view): Kitchen 1080p at threshold 0.01, 17.4 to 12.8 s, same image. Not with motion blur time samples (they run to `eldr:filmSamples`). |
| Denoiser (OIDN) | `eldr:denoise` (off); Karma's `karma:hydra:denoise` or a `denoise` entry in `karma:global:imagefilter` | Intel Open Image Denoise 2.x (Apache 2.0, optional: `OIDN_ROOT`, default C:/dev/oidn-2.5.1; the host's own DLL, e.g. Houdini's, wins) cleans the converged film once on the CPU, with the film's albedo and normal as guides. The film then uses a box pixel filter and the final gather skips its own filter (OIDN keeps correlated noise as texture). The undenoised beauty is the `eldr:beautyRaw` AOV. 1080p: about 1.3 s per frame (GTX 1080 Ti machine; OIDN's CUDA device needs Volta or newer). 16 samples denoised beat 64 raw ones (box filter, vs a 1024-sample reference: Kitchen 0.0047 vs 0.0058 RMS). Light AOVs stay raw (they sum to the raw beauty). |
| Per-component quality | `eldr:diffuseQuality`, `eldr:reflectQuality`, `eldr:lightQuality`, `eldr:sssQuality`, `eldr:volumeQuality` (unset: Karma's `diffusequality`, `reflectquality`, `light_sampling_quality`, `sssquality`, `volumequality`, else 1) | Rays where the noise is, not more film samples for everything: final gather, GI probe and AO rays; traced reflection rays; traced shadow rays per light; subsurface taps; volume shadow steps, each times its quality. Under the film a fraction spreads over the samples (0.5: traced every other sample, which keeps the last sample's image; 1.5: 1, 2, 1, 2 rays; Sobol points run on), so the converged image is unbiased. Interactive and with motion blur time samples: rounded, at least 1. A kept image under lens (DOF) jitter belongs to the last sample's lens point: slightly softer blurred shadows. Read from the settings, not per object. |

- Stochastic effects use per-effect Sobol dimensions with blue-noise rotation (`eldr:sampleDimensions`). RMSE against a 1024-sample reference, 64 samples: sunangle 0.0034 to 0.0003, fog 0.0052 to 0.0004, Kitchen_set 0.0068 to 0.0004; 0.2-0.5 s.
- Ids, depth and surface AOVs (normal, st, P, camera depth) are the sample nearest the pixel centre, not averages.
- Colour AOVs (albedo, emission, light splits) are filtered like the beauty, so the splits keep summing to it. Without the film (TAA) they are a running mean.
- Per-AOV sample filters (`eldr:aovFilters`, below) change either default per render var.

## Lighting and shadows

Lights are UsdLux: radiance = `color * intensity * 2^exposure`, times blackbody if `enableColorTemperature`, divided by area when `normalize`.

| Light | Support |
|---|---|
| DistantLight | The sun; `inputs:angle` gives soft penumbrae. The first shadow-casting one gets the cascades. |
| SphereLight | Energy-conserving representative-point specular, horizon-clipped diffuse, `treatAsPoint` |
| RectLight, DiskLight | Exact polygon form factor (rect) or illuminance (disk); emit from -Z; `inputs:texture:file` textures them (Karma orientation: left edge at +X, top row at +Y) |
| CylinderLight | Side only; `treatAsLine` |
| DomeLight | Latlong `inputs:texture:file`, rotation; diffuse SH9 and GGX-prefiltered cube |
| Shaping | Cone angle, softness, focus, focus tint; IES profiles (`shaping:ies:file`, `angleScale`, `normalize`) |

- **Highlights of sized lights** are energy-conserving: reflected power, peak and shape match a brute-force GGX integral within 5% (and Karma) for sphere, rect, disk, sun and cylinder. Diffuse is unchanged. Textured lights are read at the lobe's mip.
- **Clustered lights:** up to 255 per cluster (a fuller cluster shades every local light, never drops one), cut off at 1% of mid-gray on a white surface.
- **Emissive meshes** (constant `emissiveColor` above `eldr:emissiveLightThreshold`, small, max 32) also become rect or sphere lights with shadows. A flat one (a panel) is a rect on each face and, unless thinner than 1% of their area, on each edge: beside a panel under a ceiling the edges give most of its light.
- **Light shapes** are hidden from the camera unless `eldr:lightShapesVisible`; `inputs:karma:light:renderlightgeo = 0` hides one. The same attribute on a dome hides it from the camera (unauthored: shown, unlike Karma) while it still lights and reflects; `eldr:domeCameraVisible` and Hydra's `domeLightCameraVisibility` hide every dome.
- **Dome sun** (`eldr:domeSun`): bright HDRI texels become a shadowed distant light.
- **Light linking** (`eldr:lightLinking`): a light's `collection:lightLink` limits what it lights, its `collection:shadowLink` what casts its shadows (UsdLux; Houdini's Light Linker LOP writes both). Hydra's light linking scene index turns them into categories; each instance and volume carries a 32-bit mask of the linked collections it is in. Every light loop honours the light link (raster, clusters, traced hits, GI probes, volumes, subsurface transmission), the shadow link every shadow (shadow maps and atlas tiles, translucent and volume tint maps, traced shadows, a volume's own shadow). Light groups still sum to the beauty. Up to 32 linked collections (lights sharing a collection share one); lights past them light everything, with a warning. Domes and the fog ignore links; per-instance links work for one level of instancing (as Hydra's scene index).
- **IES profiles** (`eldr:lightIes`): `inputs:shaping:ies:file` (LM-63, type C) scales a sphere, rect, disk or cylinder light per direction wherever it lights: raster, traced hits, GI, fog and volumes. Theta is measured from the light's -Z (its emission axis), phi 0 at its -X (as Karma; the direction phi grows in is not checked); `angleScale` rescales theta as UsdLux says; `normalize` divides by the profile's mean intensity (over the sphere, or the hemisphere it covers), else candela as is (as Karma; hdEmbree is 14x darker). Baked into a 64 x 256 half-float texture per file. As Karma, the profile is taken from the whole light where the light covers a pole (theta 0 or 180, where phi wraps): a profile whose pole values differ by phi blurs over the light instead of pinching to a dot (32 taps there, one elsewhere).
- **Clamps:** `eldr:directClamp` (0 = off) and `eldr:indirectClamp` limit fireflies. The indirect clamp is Karma's colour limit (default 20, 0 = off): `eldr:indirectClamp`, else (`eldr:colorLimitShared` off) `karma:global:colorlimitindirect`, else `eldr:colorLimit`, else `karma:global:colorlimit`, else 20. The `karma:` names are read from the render settings prim too (usdview, usdrecord), not only from husk's settings. It limits bounced light and the emission a bounce ray sees (GI, final gather, traced reflections and refractions), as Karma limits a sample after its path throughput; the direct clamp stays Eldr's own and off. Eldr clamps the radiance at the hit, before the surface's albedo, so a dark surface lit by a very bright emitter is limited harder than Karma's sample would be. Emissive meshes promoted to lights (`eldr:emissiveLights`) follow the indirect clamp too: Karma reaches emitters by bounce rays only and clamps each sample, radiance times albedo, so their light on a surface is scaled where radiance x albedo passes the clamp (a white wall gets about 20/0.73 of a panel of 40).
- **Colour limit on camera rays (Karma-matching):** Karma clamps every camera sample at `karma:global:colorlimit` (default 20), so an emitter of 40 seen directly renders as 20 and a (60, 30, 8) bulb as (20, 10, 2.7), hue kept (measured in Karma 22.0.466). Eldr does the same to the emission the camera sees, from the same setting: `eldr:colorLimit`, else `karma:global:colorlimit`, else 20 (-1 = unset, so Karma's value or 20 applies; 0 = off). Karma has no separate control for camera rays (H22's controls are `colorlimit`, `colorlimitindirect` and `colorlimitindlinked`), so neither has Eldr. **Full HDR emitters for comp:** set `eldr:colorLimit` to 0 (off), or Karma's Color Limit to a large value such as 1e6: Karma's 0 is not off, it renders black (husk 22.0.466), so Eldr ignores a `karma:global:colorlimit` of 0 with a warning and keeps its default of 20. To keep the bounce clamp against fireflies, also set `eldr:indirectClamp` 20 (it wins over the colour limit), or turn Karma's Shared Color Limit off and keep its Indirect Color Limit. Light shapes made visible (`eldr:lightShapesVisible`) are not capped.
- **Ambient:** GTAO on the dome's light, or with SSGI the traced visibility.

### Shadow modes

| Mode | How |
|---|---|
| Sun cascades | 4 cascades fit to the scene, texel-snapped, normal offset and slope bias. Soft from the light angle (PCSS, bounded blocker search) or 3x3 PCF. |
| Local light atlas | 4096x4096 tiles; spots one perspective tile, others six cube faces; nearest lights first. Tiles halve (to 64, about 680 lights) before any light goes unshadowed. Redraws only when scene or tiles change. Soft from light size (PCSS); blocker search and filter taps compare against the receiver's plane, so a wall beside a big light doesn't shadow itself. |
| Translucent and volume tint | Colour maps beside the shadow maps. |
| Ray-traced (`eldr:rtShadows`) | Sun + local lights up to `eldr:rtShadowLights` (4) in all, soft from light size; skinned instances capped (`eldr:rtSkinnedInstances`). Rays pass translucent casters (taking their colour) and march volumes (`eldr:rtTranslucentShadows`). Rays sample only the part of a light above the surface's horizon (the shading clips the rest), and points inside a sphere light are unshadowed as with maps, so an emissive ball resting on the floor lights it brightest at the contact. Denoised (`eldr:rtShadowDenoise`): PCSS-style, visibility averaged over the penumbra's width (from the blocker's distance and the light's size, up to 3 pixels) in the receiver's plane, so a small sun's and contact shadows stay hard; fading out over the film's first `eldr:effectDenoiseSamples` samples. Lights that give a pixel nothing (range, cone, light linking) trace no ray; from the film's 2nd sample over 4 lights share the samples per 8x8 block, weighted (`eldr:rtLightSampling`). |
| Traced when cheaper (`eldr:rtShadowsAuto`) | Traces dense scenes whose skinned instances all block rays, else maps. With `eldr:rtShadowSamples` 1 a film takes one ray per light and sample at the sample dimensions' point of the light: exact penumbra. |
| Jittered maps (`eldr:shadowJitter`) | Maps rendered from a point on the light each film sample: exact penumbra (sun error 0.016 vs PCSS 0.065). |

Traced shadows cover opaque pixels only. With volumes, fog, translucent surfaces or subsurface in the scene a traced sun keeps its cascades for those (1.05 ms on volume.usda); traced local lights keep atlas tiles for them (about 1.4 ms for three lights; still frames 0-0.2 ms).

## Global illumination

| Layer | Setting | Needs | What it gives |
|---|---|---|---|
| Dome light + AO | `eldr:imageBasedLighting`, `eldr:ambientOcclusion` | prepass | Unoccluded sky light darkened by GTAO |
| DDGI probes | `eldr:gi` | ray tracing | Probe grid (`eldr:giProbes`, `eldr:giRaysPerProbe`); octahedral irradiance and visibility atlases blended by `eldr:giHysteresis`; Chebyshev visibility prevents leaks; probes in or just behind walls move out (`eldr:giRelocation`, RTXGI's relocation), those that can't drop out; hits shaded with direct light, emission and probe bounces; dome fallback |
| Final gather | `eldr:finalGather` | GI, prepass | Off by default. `eldr:finalGatherRays` (1) cosine rays per pixel and sample, their directions a scrambled Sobol sequence over the film's samples turned per pixel by blue noise (stratified); the gathered light (before the albedo) denoised per frame by two edge-aware a-trous passes (`eldr:finalGatherDenoise`, plane, normal and luminance stops: corners keep their contact shadows within 1%), fading out over the film's first `eldr:effectDenoiseSamples` (64) samples like the traced shadows' and reflections' filters; off under `eldr:denoise`, which wants uncorrelated noise. Each ray shaded at its hit with direct light (rect lights by their exact form factor) and probe bounces (`eldr:finalGatherDistance` > 0: rays that long, else the probes at their end); replaces the probes, AO and SSGI on opaque diffuse light: contact shadows, corners, colour bleeding. Hits shadow one light picked by its light, weighted (`eldr:rtLightSampling`); optionally (`eldr:finalGatherRoulette`, off) from the film's 2nd sample each 8x8 block gathers every other sample, weighted by 2 |
| SSGI | `eldr:ssgi` | prepass | Near-field indirect light from the screen (GTAO horizon scan, last frame's lighting) at half resolution, denoised. Replaces AO. |
| Caustics | `eldr:caustics` | ray tracing | Off by default; only with caster objects. See below. |

### Caustics

Light focused by glass or metal onto diffuse surfaces (a glass's bright spot, a pool floor's ripples), by progressive photon mapping:

- **Casters:** objects with `primvars:karma:object:causticsenable` (Karma's Enable Caustics; `primvars:eldr:object:causticsenable` wins). None by default, so nothing is traced. Their glass refracts and reflects photons by Fresnel (absorbing as traced refraction), their metal reflects them (Schlick from the base colour); the rest of a caster is diffuse. Roughness is at least `karma:object:causticsroughnessclamp` (default 0.2, as Karma; `eldr:` twin).
- **Lights:** distant, sphere (spot cones included), rect and disk lights send `eldr:causticsPhotons` (65536) photons each per film sample, aimed at the casters' bounding sphere; `inputs:karma:light:contributescaustics` (or `inputs:eldr:light:contributescaustics`) off leaves a light out. Not cylinders, emissive meshes, domes (their extracted sun does send) or light textures.
- **Receivers:** every mesh but refracting glass gathers them on its diffuse surface; `primvars:eldr:object:causticsreceive` overrides. Photons are kept in a hashed grid on the GPU (2^20 cells, a cell twice the gather radius).
- **Progressive:** film sample i gathers within r_i, r_(i+1)^2 = r_i^2 (i + 2/3) / (i + 1) (Knaus and Zwicker 2011), from `eldr:causticsRadius` (0: 2% of the casters' bounding radius); the film's average converges, sharper with every sample. Adaptive tiles don't skip tiles while caustics run.
- **Shadows:** under caustics a caster's glass blocks its traced shadows and tint maps (its light arrives as caustics instead, no double counting), with `eldr:translucentShadows` and `eldr:rtTranslucentShadows` off too.
- **AOV:** `caustics` (raw), or Karma-style LPEs `C<RD>.+<TS>L`, `C<RD><TS>.*L`, `C<RD>[<TS><RS>].*L`; it is part of indirect diffuse, so the splits still sum to the beauty. Traced hits (reflections, refraction, GI probes, final gather) see caustics too.

Against Karma (tests/scenes/caustics.usda, karma_golden at 320x180: Karma CPU 1024 samples with the ball's Enable Caustics and its colour limit raised, Eldr 64 film samples; 2026-10-29): the focused patch is 0.90-0.97 of Karma's per 10 x 10 pixel cell (13.7 vs 14.2 at the peak), the sunlit floor 0.547 vs 0.540, image mean luminance 0.99. With Karma's default colour limit its caustics are much dimmer: the limit clamps every sample that sees the sun through the glass. Cost (GTX 1080 Ti, compute fallback, 960x540, the same scene): 5.9 ms per film sample off, 13.3 ms with 65536 photons, 34.4 ms with 262144 (the photon trace dominates; 65536 and 262144 give the same patch within 1% at 64 samples). A photon pass gathered on a cap of 4096 photons per pixel lost half the focus; the cap is now 65536 (a hang guard).

- `eldr:ssgiBitmask`: 32-sector bitmask, light passes behind thin objects (occluder thickness `eldr:ssgiBitmaskThickness`).
- `eldr:ssgiThickness`: rays respect thin and grazing surfaces.
- `eldr:screenBackface`: back-face hits take `eldr:screenBackfaceScale` (0.25) of the light, in SSGI and SSR.
- Slices are normalized so a white furnace never exceeds the open sky.
- `eldr:emissiveLights`, `eldr:fogGiAmbient`, `eldr:probeNormalize` use GI.
- `eldr:giPreconverge`: the probes' about 33 updates run in the first frame.

Costs at 1280x720: probes 2-3 ms (Kitchen), SSGI 0.67 ms at half resolution.

## Reflections

Each pixel takes the best source for its roughness.

| Source | Setting | How |
|---|---|---|
| SSR | `eldr:ssr` | 32-step ray march with bisection, shaded from last frame; fades with roughness (0.3-0.6) and at edges. `eldr:ssrHitFilter` reads hits at the ray cone's mip (6 mips). Hits outside last frame's view are misses. |
| SSR denoiser | `eldr:ssrDenoise` (off) | One GGX ray per pixel, 9-ray spatial filter, temporal reprojection, bilateral filter; roughness up to 0.8. Crowd shading 11.7 to 20.4 ms. Not with ray-traced reflections. |
| Reflection probes | `eldr:reflectionProbe` | Captured cube with parallax correction. One automatic probe, or up to 8 authored (`primvars:eldr:reflectionProbe`; smallest box wins, soft edges). `eldr:probeNormalize` needs GI or SSGI. |
| Planar reflections | `eldr:planarReflections` | Mirrors (`primvars:eldr:planarReflection`, else largest flat glossy opaque mesh) render the scene mirrored with an oblique near plane; `eldr:planarCount` (4) at `eldr:planarScale` (0.5). |
| Ray-traced reflections | `eldr:rtReflections` | [RT] Off by default. GGX rays shaded at hits (shadowed by every light with `eldr:rtHitShadows`), up to roughness 0.7, replace dome and SSR; rough ones averaged over up to 12 pixels (`eldr:rtReflectionDenoise`, fading out over the film's first `eldr:effectDenoiseSamples` samples). Surfaces with specular weight 0 trace none; from the film's second sample dim ones (dielectrics seen head-on, not near-mirrors) trace in a share of the samples (at least 1/8) per 8x8 block, weighted to match (`eldr:rtReflectionRoulette`). Sees displacement (`eldr:rtDisplacement`) and skinned instances up to the cap. |

Mirror pixels take the planar reflection; others use the probe (if on) or the dome as the base, with SSR where the screen has the hit.

## Transparency

| Feature | Setting | How |
|---|---|---|
| Cutouts | | UsdPreviewSurface `opacityThreshold > 0`, MaterialX opacity textures at 0.5; alpha-tested in every pass, shadows too |
| Translucency | `eldr:translucency`, `eldr:oit` | Opacity below 1 (constant, or a texture without threshold). Drawn after the sky, weighted blended OIT (`eldr:oitDepthWeight`) or sorted per mesh. Refracting and other translucent meshes share one back-to-front order: the meshes between two refracting ones are one OIT group, resolved before the nearer glass. Diffuse and emission scale with opacity, highlights don't. |
| Refraction | `eldr:refraction` | [prepass] Transmission (standard_surface `transmission*`, OpenPBR `transmission_*`) or authored `ior` refract a mip-chained copy of the opaque colour. Thickness estimate places the exit point; roughness blurs (`eldr:refractionSamples`); Fresnel splits; absorption at the surface or Beer-Lambert over `transmission_depth`. |
| From inside | `eldr:refractionBackFaces` | Back faces first with inverted ior: Snell's window, total internal reflection (48.8 degrees for water) |
| Layers | `eldr:multiRefraction`, `eldr:refractionLayers` | Background re-copied before each refracting layer: glass behind glass, and translucent meshes behind glass, refracted. Thin walls skip the back-face pass (and its copy) |
| Ray-traced refraction | `eldr:rtRefraction` | [RT] Off by default. Traces the nearest refracting surface through up to `eldr:rtRefractionBounces` interfaces (total internal reflection, Fresnel choice, absorption, GGX normal per sample for rough glass). Materials are evaluated at each hit (textures, programs, normal maps); shaded like traced reflections, so off-screen content shows. Rays that leave the scene see the dome and the distant lights' discs (radiance = irradiance over the disc's solid angle, times the light's specular, limited by the indirect clamp as Karma's colour limit; Karma's sun through a glass ball). Reflections, GI and final gather leave the sun to direct lighting, so it isn't counted twice. Counts as transmission in the light splits (Karma's `C<TG>.*L`). Displaced glass bends rays by its displaced triangles and height-map bump (`eldr:rtRefractionDisplacement`). The first ray of a pixel looks for glass only, up to the opaque depth (`eldr:rtRefractionClip`). |
| Translucent shadows | `eldr:translucentShadows` | Glass casts faint, tinted shadows |
| Holdouts | `eldr:holdouts`, `eldr:holdoutAlpha` | Matte objects: black, zero alpha, keep depth, ids, shadows. Translucent holdouts cut by opacity, also from MaterialX graphs. |
| Subsurface | `eldr:subsurface` | See Materials |

## Volumes and fog

**Fog** (`eldr:fog`): height fog in a 160x90x64 froxel grid; medium = `density * exp(-falloff * (height - base))`, Henyey-Greenstein phase, lit by distant lights (cascades: pillars throw shafts), local lights (atlas tiles) and the dome (probes if `eldr:fogGiAmbient`). Energy-conserving; applied to surfaces and sky. No USD fog schema: settings only.

**OpenVDB volumes** (`eldr:volumes`; needs a USD with OpenVDB):

| Aspect | Support |
|---|---|
| Prims | `UsdVolVolume` with `OpenVDBAsset` fields, bound by name; `fieldIndex` honoured. Without a volume material only `density` is read and the medium is Karma's: scattering 1 per unit of density, no absorption, isotropic (measured equal to Karma CPU's `anisotropic_vdf(0, density, 0)`); `eldr:volumeDefaultFields` adds `temperature` (blackbody), `Cd`/`color` and `emission` |
| Rendering | NanoVDB on the GPU, fullscreen march over the shaded image, empty space skipped |
| Lighting | HG phase, single scattering as Karma's default (multiple scattering of the sun and dome as octaves fitted to Karma's Volume Limit, with `eldr:volumeMultipleScattering` or a volume limit; local lights stay single scattering), sun cascades for opaque occluders, the dome's upper half from SH (Karma's single scattering over a ground; geometry doesn't block the dome), local lights as points via atlas tiles; emission from temperature (blackbody) or an emission field |
| Materials | MaterialX `volume` (anisotropic_vdf, absorption_vdf, uniform_edf) or Karma's `kma_volume`. Inputs: constants, `geompropvalue` field reads and their products (`constant`, `multiply`, `convert`, `dot`), blackbody of a temperature field. Absorption and scattering are per channel: coloured smoke, coloured transmittance (half resolution too) and coloured shadows on surfaces, fog and other volumes. Coefficients are per unit of density. A colour field scales the scattering; another field times a colour is emission. `eldr:volumeAnisotropy` for volumes without one. |
| Ramps and remaps | Functions of one float field each, up to three per material, baked into 1024-entry tables over the field's value range (linear in between) and multiplied in. A function of the density replaces the density (a ramp of it is the coefficient); a function of temperature times a colour is fire. Nodes, as MaterialX's stdlib defines them: `remap`, `range`, `smoothstep`, `clamp`, `mix`, `add`, `subtract`, `multiply`, `divide`, `power`, `min`, `max`, `modulo`, `invert`, `absval`, `floor`, `ceil`, `sign`, `sqrt`, `exp`, `ln`, `ifgreater`, `ifgreatereq`, `ifequal`, `switch`, `extract`, `combine2/3/4`, `luminance`, `ramp` (linear, smooth, step; standard, radial, circular, box), `ramplr`, `ramptb`, `ramp4`, `splitlr`, `splittb` (a hard step: no derivatives in a volume), a field into `texcoord` through `convert` or `combine2`. Houdini's: the MtlX Color/Float Ramp and Cubic Ramp VOPs (`hinvlinear`, `huniformramp`, `huniformcubic`, `hcatmullrom`), Karma's `kma_rampconst` (every Houdini basis: constant, linear, Catmull-Rom and monotone cubic flat at the end keys, bezier and bspline as curves through the keys). A node of two fields, or any other node: the material is ignored with a warning. Measured against Karma CPU (husk, Houdini-authored networks): emission and absorption within 5% per channel (a Catmull-Rom ramp's faint blue 14%); lit scattering (single, as Karma's default) within 3% for ramps of the density. |
| Fields | Density: the field bound as `density`, else a nameless one (`field_N`, SOP Import's name for an unnamed volume), else the first float field with no other role. Vec3 VDB grids load as vectors. A file several fields read (density, temperature, vel of a pyro frame) is read once per sync; a field whose file, grid and modification time didn't change isn't read again. |
| Velocity blur | Karma's Volume Velocity Blur in its Lookup Offset mode: with Velocity Blur on (`primvars:eldr:object:vblur`, else `karma:object:vblur`, above 0) and motion blur time samples, the fields are read at `p - vel * t` (`vel` a Vec3 grid, or Houdini's `vel.x`/`.y`/`.z` float volumes; else `v`), units per second, times `volumevblurscale`. Not Karma's default Lattice Deformation: features that overlap after the blur, and velocities outside the velocity field's active voxels, differ. `eldr:volumeVelocityBlur` |
| Houdini SOP volumes | Houdini build only; see the Houdini page |

| Interaction | Setting |
|---|---|
| Volumes shadow surfaces and fog from the sun | `eldr:volumeShadows` |
| ... from local lights | `eldr:localVolumeShadows` |
| Volumes shadow each other (sun, local lights, dome); a lone volume pays nothing | `eldr:volumeMutualShadows` |
| Glass tints fog; volumes behind glass are dimmed by every translucent layer in front of them (up to `eldr:refractionLayers`, at most 4, peeled per pixel) and refracted | `eldr:volumeUnderTransparency` |
| Half-resolution march, about 3x cheaper | `eldr:volumeResolution` 2 |
| Local lights' shadows (atlas tap, other volumes) every second march step, the steps between reuse the last tap's shadowed fraction | `eldr:volumeLocalShadowStride` |
| Shadow grids: each volume's optical depth towards the dome, the suns and the local lights (other volumes) built once per change on a 64^3 grid and read trilinearly instead of shadow marches (3-4x cheaper volumes; time samples of velocity-blurred volumes march) | `eldr:volumeShadowGrid` |

## Materials

Eldr shades in linear Rec.709. Colour-space tags (`sourceColorSpace`, UsdColorSpaceAPI) convert from ACEScg, ACES2065-1, P3, Rec.2020, sRGB and gamma encodings; unknown names warn once and read as untagged.

| Surface | Notes |
|---|---|
| UsdPreviewSurface | Metallic and specular workflows, clearcoat (F0 from `ior`), `ior` refraction, opacity, cutouts, displacement, normal maps |
| MaterialX standard_surface | base, specular (`specular_color`; both may be graphs, e.g. Houdini's Silk facing-ratio ramp), roughness, metalness, coat, sheen, thin film, anisotropy, transmission, subsurface, emission, opacity, normal maps, displacement |
| OpenPBR Surface | The same lobes under OpenPBR names (`fuzz_*`, `coat_*`, `thin_film_*`, `geometry_opacity`, `transmission_*`, `subsurface_*`) |
| MaterialX gltf_pbr | standard_surface's lobes: base, metallic, roughness, normal, `specular`/`specular_color`, `ior`, clearcoat, sheen (`sheen_color`'s largest channel is the weight), iridescence (thin film), transmission (tinted by `base_color`, or `attenuation_color` at `attenuation_distance`; thin walled at `thickness` 0), anisotropy, emissive, `alpha` by `alpha_mode` (opaque ignores it, mask cuts out at `alpha_cutoff`, blend). The metal's edge follows `specular` as standard_surface's does |
| MaterialX disney_principled | UsdPreviewSurface's Schlick lobes: dielectric F0 0.08 `specular`, metal, sheen tinted by `sheenTint`, clearcoat (F0 0.04, roughness 1 - `clearcoatGloss`), `specTrans` (with `ior`), subsurface, anisotropy. Lambert diffuse (the graph's is Burley); `specularTint` isn't read |
| MaterialX surface_unlit | `emission_color` times `emission`, cutout `opacity`; no reflection. `transmission` isn't read |

Material context order: `mtlx`, Storm's `glslfx`, then universal (`outputs:surface`). Surfaces Eldr doesn't read (a `surface` of pbrlib BSDFs and EDFs) render the default grey and warn once; docs/materialx.md has the measured node coverage.

### Spec conformance

Eldr shades each surface model by its spec: the USD spec for UsdPreviewSurface, MaterialX's nodegraphs and pbrlib (`libraries/bxdf`, `libraries/pbrlib/genglsl`) for standard_surface and OpenPBR (`renderer/shaders/brdf.glsl`'s `SetupBase`). Checked lobe by lobe against the spec formulas: tilted patches under one distant light (9 light and view directions per case, 23 cases) agree within 0.3% (worst 2.5%), and under a white furnace dome within 1% facing the camera. Differences from Karma come from rasterised lighting, not the materials (Karma agrees with the spec within 1% on coated standard_surface and OpenPBR, and on furnace albedos).

| Lobe | Spec | Status |
|---|---|---|
| GGX distribution, height-correlated Smith masking, alpha = roughness² | all models | Exact (roughness under 0.045 shades as 0.045, alpha 0.002, so point lights keep a finite highlight) |
| UsdPreviewSurface Fresnel | Schlick: F0 = ((1 - ior) / (1 + ior))² to white; metals albedo to albedo; interpolated by `metallic`; specular workflow `specularColor` to white | Exact |
| Diffuse under the specular | MaterialX `layer`: the diffuse loses the dielectric's directional albedo towards the viewer | Exact (albedo from the split-sum table) |
| Multiple scattering | MaterialX's energy compensation on every GGX lobe of standard_surface and OpenPBR; UsdPreviewSurface single scattering (Storm, Karma) | Exact form; `eldr:multiScatter` off makes everything single scattering |
| standard_surface dielectric | exact Fresnel of `specular_IOR`, weight `specular`, tint `specular_color` | Exact for lights; image lights use Schlick's albedo at F0, as MaterialX's GLSL (1-3% under exact at 50 degrees) |
| standard_surface metal | conductor of `artistic_ior(base_color, specular_color)` | Hoffman's F82 form fitted to the conductor (exact at 0, 82 and 90 degrees; within 0.5%, a dark channel up to 6% at grazing light) |
| OpenPBR metal | F82-tint Schlick (`specular_color`) times `specular_weight` | Exact |
| OpenPBR dielectric | `specular_weight` scales F0 through the index, the index relative to the coat's under a coat | Exact for lights; image lights as standard_surface's |
| Diffuse roughness | standard_surface: MaterialX's Oren-Nayar; OpenPBR: energy-compensated Oren-Nayar (Fujii) | Exact for lights (area lights: towards their centre); image lights through MaterialX's albedo fits |
| Coat | dielectric of the coat's IOR (UsdPreviewSurface: Schlick, white), coat colour on the base, albedo scaling; standard_surface `coat_affect_color`, `coat_affect_roughness`; OpenPBR roughening and `coat_darkening`; emission under the coat | Exact for lights; image lights Schlick's albedo. No `coat_normal`, coat anisotropy or coat rotation (the base's normal) |
| Sheen (standard_surface) | Imageworks microfiber over the diffuse, under the specular | Exact; image lights through MaterialX's albedo fit |
| Fuzz (OpenPBR) | Zeltner's LTC sheen over the coat and base | Exact (MaterialX's fits) |
| Anisotropy | `mx_roughness_anisotropy`, OpenPBR's mapping | Exact; tangent from the uv set's u direction (no tangent primvars) |
| Thin film | Airy reflectance with polarization, per light | Approximated: unpolarized, at the view angle, standing in for F0; on transmitting surfaces what the film reflects no longer gets through (a soap bubble's colours) |
| Transmission | dielectric BTDF under the coat (its colour and reflection attenuate it), absorption by `transmission_color` over `transmission_depth` | Approximated: refraction of the screen (or traced), Schlick at the interface, roughness as blur, path length from the depth behind (capped by the object's bounds) |
| Subsurface | random walk | Approximated: separable screen-space blur of the diffuse light |
| Constants only | `diffuse_roughness`, `base_diffuse_roughness`, `coat_affect_*`, `coat_darkening` | Textured or graph-driven values keep the constant |

UsdPreviewSurface `opacityMode` "presence" (without a threshold) scales the whole lighting response by the opacity, highlights included, and never refracts; "transparent" (the default) fades diffuse light and emission only (`eldr:opacityMode`). Not mapped: presence in ray-traced hits. Ray-traced hits (GI rays, reflections of reflections) shade with a simplified F0 = 0.04 / metallic base. Traced reflections, the final gather and traced refraction's last hit run a hit's material program first (its base colour, emission, metallic, roughness; with `eldr:materialGraphs`), so a graph-coloured neighbour reflects in its colour; GI probe rays read only constants and textures.

| Topic | Support |
|---|---|
| Lobes (own pipelines; plain materials don't pay) | clearcoat (tints the base), sheen (Imageworks; OpenPBR fuzz: Zeltner), thin film (Airy), anisotropic GGX (tangent from the uv u direction, `specular_rotation`), hair (Kajiya-Kay on BasisCurves), toon bands, multi-scattering energy compensation (`eldr:multiScatter`: standard_surface and OpenPBR; UsdPreviewSurface single-scatter, as Karma) |
| Texturable inputs | base colour, roughness, metallic, emission, opacity, normal, displacement; with `eldr:texturedLobes` / `eldr:texturedTransmission`: specularColor, coat (colour, roughness), sheen, anisotropy, thin film, ior, transmission (colour, depth, thin walled), subsurface (colour, radius, scale) |
| Texture reads | UsdUVTexture, MaterialX `image` / `tiledimage`, UDIM, wrap modes, uv transforms, several uv sets |
| Texture compression (`eldr:textureCompression`, off by default: exact textures, as Karma never compresses) | 8-bit textures become BC7 (colour, sRGB stays sRGB; normal maps too: BC5 would be the same size and need a shader path), BC4 (gray) or BC5 (gray + alpha), with box-filtered mips (sRGB in linear), encoded on the CPU once and kept in `%LOCALAPPDATA%\Eldr\textures` (or `HDVK_PIPELINE_CACHE_DIR`), keyed by path, size, modification time and colour space. BC7 uses mode 6 only (one colour line per 4x4 block). No BC1 (block artefacts) and no BC6H: 16-bit and float textures (displacement, HDR) and light textures (the dome is read on the CPU) stay exact. The cache folder is never pruned. |
| Material programs (`eldr:materialGraphs`) | Graphs compiled whole into per-pixel programs: math, mix, overlay, clamp, remap, conditionals, HSV, extract/combine, `image`, `tiledimage`, `place2d`, `texcoord`, `geompropvalue`, ramps, `normalmap`, UsdUVTexture, UsdTransform2d, primvar readers, Houdini ramps `hinvlinear` and `huniformramp`. Compositing: plus, minus, difference, burn, dodge, screen, overlay, disjointover, in, mask, matte, out, over, inside, outside, premult, unpremult (genglsl's formulas; burn and dodge read 0 where they would divide by 0). Karma's own nodes: `kma_rampconst` (the ramp baked into a 64-float or 32-colour table over its keys), `kma_color_hextiling` (Houdini's hex tiling: three rotated, scaled, offset tiles blended by weight, falloff and contrast; with `eldr:proceduralTextures`; no `direction`, no `weights` output), `kma_voronoinoise3d` (F1, F2, F3 distances at `position * freq - offset` with the four metrics; MaterialX's cell hash, so other cells than Karma's; unconnected `position` is the object-space point; no `p1`..`p3`). Also fract, round, sign, tan, asin, acos, atan2, magnitude, crossproduct, rotate2d/3d, switch (a constant `which` compiles one input), and/or/not/xor, splitlr/splittb, time/frame (the scene's current frame), geomcolor, tangent/bitangent, viewdirection, artistic_ior, transformmatrix (constant matrices), transformpoint/vector/normal. Nodes defined by a nodegraph (MaterialX's stdlib/pbrlib/nprlib, Houdini's MaterialX library when `$HFS` is set, a document's own nodedefs) are expanded into the nodes Eldr compiles: `contrast`, `colorcorrect`, `ramp4`, `range`, `safepower`, `bump` (with `heighttonormal`, from screen derivatives; flat per vertex and at ray hits; Houdini (Karma context) materials read bump heights in scene units, as Karma), `triplanarprojection`, `unifiednoise3d`, `kma_camerafalloff`, ... Unconnected inputs read their nodedef's `defaultgeomprop` (UV0, Nworld, Pobject). With `eldr:proceduralTextures`: MaterialX noise (noise2d/3d, fractal2d/3d, cellnoise, worleynoise, position, normal), matching Storm. A node Eldr can't compile is bypassed as MaterialX bypasses it (its `defaultinput`, else its first input of the output's type, else the output's default); without a nodedef the input keeps its constant (one warning per node type). Render stats `materialFallbackInputs` and `materialFallbackNodes` (usdview's Eldr > Render Statistics, Houdini's viewport stats) count those inputs and name the nodes; unknown surface shaders count too and warn once. |
| Geometry-side | per-vertex `displayColor`, GeomSubsets with materials, any primvar a program names at any rate (including instancer primvars such as Houdini crowd `random`) |
| Subsurface | Separable per-channel Gaussian of the diffuse light (taps over 3x the largest radius, max 128 px, depth-aware); thin parts transmit the sun, thin-walled pass it whole |
| Toon | `inputs:eldr:toonBands` on a material, or `eldr:toonBands` globally; `inputs:eldr:toonSoftness` edges; emission untouched |

## Geometry

| Feature | Support |
|---|---|
| Meshes | Implicit surfaces converted to meshes, GeomSubsets, PointInstancer and native instancing (one indirect draw per prototype), render tags and purposes |
| Subdivision (`eldr:subdivision`, `eldr:maxRefineLevel`, `eldr:screenDicing`) | catmullClark, loop, bilinear via OpenSubdiv at the display style's refine level, a mesh's own (`primvars:eldr:refineLevel`), or with `eldr:screenDicing` (on) the level for its size on screen (Karma's `dicingquality`; husk waits a sync for the re-dice, an interactive view re-dices when the camera stops); creases, corners, holes, boundary and face-varying rules; limit positions and normals. Skinned meshes refine in the rest pose, skin on the GPU. |
| Displacement (`eldr:displacement`) | UsdPreviewSurface and MaterialX displacement move vertices along normals, so depth, shadows, motion and ray tracing agree; shading normal bumped by the height slope. Best on subdivided meshes. Culling bounds grow by the largest offset; a graph's is taken as |scale|, or the shader's `inputs:eldr:displacementBound` (object units) when the graph outputs more than -1..1. Height maps read at the mip matching vertex spacing. |
| Displacement graphs (`eldr:displacementGraphs`) | A shading graph runs per vertex, also for ray-tracing BLASes (`eldr:rtDisplacement`). Graphs reading world position, normal or instance primvars give the 256 largest instances a BLAS each (rebuilt when they or their primvars change); others share the first instance's. |
| LOD (`eldr:lod`, `eldr:lodBias`) | Meshes of 1000+ triangles get up to 3 coarser levels (quadric simplification weighing normals, uvs, colours, joint weights); culling picks the coarsest level under about 1 px of error, with hysteresis. Crowd 54.5M to 13.6M triangles, 68 to 28-34 ms; Kitchen at Very High 34.8M to 6.4M, 30 to 10.5 ms. `eldr:lodFade` crossfades over 8 frames. |
| Skinning (UsdSkel) | GPU linear blend or dual quaternion (`primvars:skel:skinningMethod = "dualQuaternion"`), 4 or 8 influences, blend shapes, per-instance poses (one draw per prototype); authored normals kept. With `HD_ENABLE_DEFERRED_SKINNING=1` (USD's, set before the app starts) instanced agents with their own animations stay one instanced mesh per prototype; a mesh with its own `skel:joints` gets every instance's joints remapped (USD remaps only the first); a skeleton is posed once per shutter time for all its meshes |
| Curves (BasisCurves) | linear, bezier, bspline, catmullRom; 6-sided tubes or flat ribbons (with normals); `widths`, `displayColor`, materials, instancing; u runs along the curve. 100k hair curves in about 2 s. `eldr:curveRibbons` / `primvars:eldr:ribbons`: camera-facing ribbons (2 triangles per step instead of 12). |
| Points | Icosahedra `widths` across, or camera-facing discs with `eldr:pointSprites` / `primvars:eldr:sprites` |
| Volumes | See Volumes and fog |

## Camera

Reads UsdGeomCamera `focalLength`, `horizontalAperture`, `fStop`, `focusDistance`, `shutter:open`, `shutter:close`. usdview's free camera has no lens (no DOF or blur).

**Exposure** (`eldr:cameraExposure`, on): the camera's `exposure`, `exposure:iso`, `exposure:time`, `exposure:fStop` and `exposure:responsivity` scale the image by USD's linear exposure scale (`responsivity * time * iso / 100 * 2^exposure / fStop^2`, `HdCamera::GetLinearExposureScale()`), as Karma: the beauty and the light AOVs (emission, the diffuse, glossy and other splits, light groups); data AOVs (depth, normals, positions, ids, albedo) stay. On the beauty it is EV added to `eldr:exposureCompensation`, so it applies before tonemapping and, with auto exposure on, offsets the metered exposure as compensation does. The colour limits (`eldr:colorLimit`, `eldr:indirectClamp`, `eldr:directClamp`, Karma's `colorlimit`) clamp exposed values, as Karma's do: in scene units they shrink by the scale. Not with `eldr:postProcess` off (raw HDR). Changing it restarts the film. usdview's free camera has none. Measured against husk Karma (lane 67, 320x180): Cornell box and emissive with Karma's colour limit off, every attribute (exposure 2, ISO 400, exposure:fStop 2, exposure:time 0.5, responsivity 2) gives Karma's beauty and diffuse-AOV ratio within 0.1%; at Karma's default colour limit within 2% on Cornell, 3% on emissive (where the limit clips most of the light at exposure 2: both renderers gain about 1.03x, not 4x).

| Feature | Interactive | Film |
|---|---|---|
| Depth of field | Thin-lens gather on a golden-angle spiral before TAA, with a 32x32 near-field tile pass (foreground spreads, background never bleeds over sharper foreground) | `eldr:dofJitter` moves the camera across the aperture per sample (sheared, focus plane stays): true bokeh with occlusion. `eldr:dofResidualBlur` runs post DOF at 1/sqrt(samples) of the blur to fill gaps. |
| Motion blur | McGuire reconstruction along motion vectors (camera, objects, skinning, blend shapes) after TAA | `eldr:motionBlurTimeSamples` renders the whole scene at a Halton-spread shutter time per sample (box shutter, linear motion from the previous frame; instances, joints, blend shapes, camera), so shadows, rays, refraction and DOF see the motion. The camera's motion is only ever its own: a switch to another camera, or Houdini's free camera (`/__HoudiniFreeCamera__`) taking over from a scene camera, keeps the objects' motion but never blurs from the old camera's pose. 1.7-2.1 ms per sample on the crowd. `eldr:motionBlurResidual` spreads each sample over its shutter stratum along motion vectors for a smooth blur. Reflections: traced ones (`eldr:rtReflections`) see the sample's TLAS; planar mirrors render at the sample's time and camera (`eldr:planarMotionBlur`); reflection probes stay at the frame's time (a capture per sample would cost six renders and a CPU prefilter). |
| Single-frame blur | | `eldr:sampledMotion`: rigid prims, instances and camera take motion from the scene's transform samples across the shutter. `eldr:deformationBlur`: point caches (meshes, curves, points) use per-point velocity. Velocity blur (`primvars:eldr:object:vblur` / `instance_vblur`, else Karma's `karma:object:vblur` / `instance_vblur`; 0 off as Karma, 1 velocities, 2 with accelerations) resolves `velocities`, `accelerations` and `angularVelocities` into those samples (USD's HdsiVelocityMotionResolvingSceneIndex). Skinned meshes sample their skinning transforms, skeleton placement and blend shape weights across the shutter too (extrapolated to one frame before, as rigid transforms are), so a first frame's skinning blurs in time samples and motion vectors. Toggling a motion blur setting blurs again at once. |
| Upscaling | `eldr:renderScale` < 1 (min 0.25), and `eldr:movingRenderScale` (0.5) while the camera or scene moves: TAA reconstructs a full-size history; post DOF and motion blur work at the render size. Film, `eldr:filmRestart`, lens distortion and extra AOVs render full size. | |
| Moving camera mode | While moving: half render size (`eldr:movingRenderScale`), one traced shadow ray per light (`eldr:movingShadowRays`), settled GI probes paused (`eldr:movingGiPause`); the first still frame is full quality again. | Kitchen 1080p moving 21.6 to 8.6 ms, emissive RT 64 to 12.5 ms |

**Lens distortion:** camera attributes `lensDistortion:*` warp the finished frame with ids and depth; the scene renders wider (overscan, up to 2x). A Karma Physical Lens (`karma:camera:lensshader`, or a bound lens material) is read, so Karma cameras distort the same.

## Output

**Hydra AOVs** (usdview AOV menu, usdrecord, husk): `color` (RGBA16F, linear unless the tonemapper runs), `depth`, `primId`, `instanceId` and the extra AOVs below. They cost nothing unless bound. Empty pixels are 0. A bound buffer returns to the CPU only if the app read it since the last frame. While the camera moves, extra AOVs the app didn't read last frame are skipped; stills, offline renders and render products render all.

**Extra AOVs.** UsdRender source names match Houdini Karma's standard render vars:

| AOV | Names | Content |
|---|---|---|
| Normal | `normal`, `ray:hitN`, `ray:hitN_facing` | World-space shading normal, camera-facing |
| Camera normal | `Neye`, `ray:hitN_camera` | Camera space |
| Position | `ray:hitP` | World space |
| Camera position | `Peye`, `ray:hitP_camera` | Camera space |
| Depth | `ray:hitPz` | Distance along the view axis, positive |
| Camera depth | `cameraDepth` | Camera-space z (negative in front, like hdEmbree) |
| st | `primvars:st` | Texture coordinates (1-3 components) |
| Base colour | `export_basecolor` | Base colour |
| Diffuse albedo | `lpe:CDA` | Base colour x (1 - metallic) |
| Emission | `lpe:CO`, `lpe:C.*O` | Emitted radiance (not in reflections) |
| Diffuse | `lpe:C<RD>L` direct, `lpe:C<RD>.+L` indirect, `lpe:C<RD>.*L` combined | Light splits, without subsurface |
| Glossy | `lpe:C<RG>L`, `lpe:C<RG>.+L`, `lpe:C<RG>.*L` or `.*[LO]` | Light splits (reflection; coat included, refraction not) |
| Transmission | `lpe:C<TG>.*L` | What glass refracts (screen-space or traced) |
| Subsurface | `lpe:C<TD>.*L` | The subsurface lobe's diffuse light, direct and indirect, blur and sun through thin parts included |
| Coat | `lpe:C<...'coat'>.*L` | The coat's reflection of lights and dome (also in glossy; the coat reflects nothing else) |
| Volume | `lpe:CV.*L`, `lpe:CVL` | Volumes' scattered and emitted light; the splits behind are seen through them |
| Shadow | `lpe:shadow;C.*[LO]` | Direct light the shadows (and the dome's occlusion) took: unshadowed minus shadowed. Karma's also counts occluded indirect light. |
| Holdout shadow | `lpe:holdouts;shadow;CDL` | The same on holdouts, diffuse only (a shadow matte, Karma's holdout_shadow) |
| Light group | `lpe:C.*<L.'tag'>` | Direct light of the lights tagged `tag` (`inputs:karma:light:lpetag`) |
| Cryptomatte object | `primvars:__name` | Ids of the rprim path |
| Cryptomatte material | `primvars:__materialname` | Ids of the material path |

A bound float buffer gets as many components as it has. One buffer per kind; if two names for a kind are bound, the last wins.

**Light splits.**

| Part | Definition |
|---|---|
| Direct | Lights' light; also the dome's unblocked light (`eldr:splitDomeDirect`): diffuse under AO/SSGI visibility, reflection where no probe, mirror, SSR or traced hit replaced it |
| Indirect | GI, SSGI, multi-bounce, probes, reflections beyond that |
| Sum | Direct + indirect = combined; with emission = the linear beauty, within 3%, for diffuse, coat, sheen, emissive, subsurface, transmission, under GI, denoised SSR and traced refraction |
| Transmission | Refracted light, its own split (Karma's `C<TG>`); not in indirect glossy |
| Denoised SSR | Indirect glossy |
| Subsurface | Its own split (Karma's `C<TD>`): the subsurface weight's share of the diffuse light, the blur's moves and the sun through thin parts; not in diffuse |
| Coat | Part of glossy (Karma counts it as a glossy reflection labelled `coat`) |
| Volumes | Their light is the volume split; every split behind a volume is multiplied by its transmittance, as Karma's paths through a medium |
| Holdouts | No light in any split (they are black); their shadow goes to the holdout shadow |
| Sum | Diffuse + glossy + transmission + subsurface + volume + emission = the linear beauty (tests/test_aov_sums.py) |
| Light groups | Up to 8 tags (`inputs:karma:light:lpetag`, Eldr's twin `inputs:eldr:light:lpetag` wins), each the direct light (diffuse, glossy, coat, subsurface) of its lights; a tagged dome adds its unblocked light (`eldr:splitDomeDirect`) and its extracted sun. All lights tagged: groups + indirect diffuse + indirect glossy + transmission + emission + volume = the beauty (subsurface materials aside: their indirect light is in the subsurface split). Indirect light isn't split per light (GI, probes and reflections gather all lights at once). Groups past 8 warn and stay black. Each group is a 16-bit RGBA layer (16 MB at 1080p). |
| Cost | Compiled in only when an AOV asks: lights scene 0.92 vs 0.62 ms |
| Accumulation | Like colour: TAA frames or film |

**Light path intensity** (`eldr:directIntensity`, `eldr:indirectIntensity`, both 1) scales direct or indirect light in the final pixel only, not the light transport or AOVs: the beauty is the splits times the intensities. Refracted light and SSR reflections are scaled once. Fog and toon shading stay unscaled. At 1 the shaders compile without it.

**EXR render products** (`eldr:renderProducts`): with an active UsdRenderSettings (`usdrecord --renderSettingsPrimPath`, husk), each raster product becomes an EXR through OpenImageIO once converged. Render vars name AOVs as UsdRender does (`sourceType` `lpe` gives `lpe:` + source name, `primvar` gives `primvars:` + it, else the source name). Files are zip-compressed float32.

| Layer | Layout |
|---|---|
| First colour var | R, G, B, A |
| Further vars | `<var>.R/G/B/A`; single channels `<var>.Z` |
| primId, instanceId | int-valued `<var>.R` |
| Cryptomatte | `<var>00`, `01`, `02` (ranks 0-5, each `(id, coverage, id, coverage)`) as Karma writes them, with spec metadata: `cryptomatte/<key>/name`, `hash` MurmurHash3_32, `conversion` uint32_to_float32, `manifest` JSON. Manifests are also in `GetRenderStats()["cryptomatte"]`. |

- **Soft Cryptomatte** (`eldr:cryptomatteSoft`, film only): per pixel, the 6 ids with the most filter weight, each with its coverage over all weight (background included), sorted. Edges between two objects sum to 1, so a decoder gets the filtered matte of the beauty. State: 14 floats per pixel per AOV (116 MB at 1080p). Interactive or off: one id, coverage 1, rank 0.
- **Per-AOV sample filters** (`eldr:aovFilters`, on; film only): a render var's `driver:parameters:aov:eldr:filter` (string), else Karma's `driver:parameters:aov:karma:filter` (its Render Var "Sample Blending"), sets how the film's samples make the AOV's pixels. Eldr's values: `beauty` (the beauty's Gaussian), `nearest` (the sample nearest the pixel centre), `gauss` and `box` with their own radius, `min` and `max` (per component), `closest` and `farthest` (the value of the sample with the least or most depth; empty samples count as farthest), each optionally followed by a radius in pixels (`"gauss 1"`; default Gauss 1.5, the others 0.5 = the pixel's own samples; at most 1.5). Karma's `["minmax",{"mode":"min"}]`, `max`, `zmin`, `zmax` map to min, max, closest, farthest; `["ubox",{}]`, which Houdini authors on every var, keeps Eldr's default (colour like the beauty, data nearest); `accumulate` and `stats` aren't supported (warning, default). A light split (or emission) with a filter other than the beauty's warns: it no longer sums to the beauty. Cryptomatte keeps its own, Hydra's `depth`/`primId`/`instanceId` stay nearest. Cost: one branch in the AOV pass; non-default data AOVs keep 2 more floats per pixel of state.
- **Size** (`eldr:productResolution`): the frame renders at the first raster product's `resolution`, `pixelAspectRatio` (by `aspectRatioConformPolicy`) and `dataWindowNDC` when that differs from the app's buffers. No authored `resolution` means 2048x1080 (as husk and Karma). The EXR display window is the resolution, the data window the rendered part. The app's buffers (colour, depth, ids, AOVs, husk's float primid and Cryptomatte) get a nearest resample.
- **Colour** (`eldr:productLinear`, on): the beauty is linear and untonemapped, exposed by the frame's exposure, no bloom. For exactly-as-rendered values set `eldr:autoExposure` off and `eldr:exposureCompensation` 0. Off: exposed, bloomed, tonemapped (ACES default).

**husk.** husk binds vars by its own channel names ("C", "depth") and carries `sourceName`/`sourceType`; Eldr resolves the AOV from that, so the table above holds under husk, which writes the EXR (Float32Vec4 colour renders as half and widens). husk (which hands Eldr its products as the `rasterRenderProducts` setting) makes the frame offline, which changes only speed (batching, readback), never the image: the settings prim's settings decide the look. Any `eldr:<setting>` attribute on the active UsdRenderSettings prim is that render setting, applied when its value changes.

## Performance

| Topic | How |
|---|---|
| GPU-driven scene | Device-local vertex, index and instance pools; only changed meshes upload. Meshes of identical content (a crowd's agents, each with its own joints) share one copy of their vertices, indices, skin weights and levels of detail: crowd 9k 1.7 GB to 0.57 GB, first frame 14 to 5 s. System memory: duplicates are found as meshes sync and keep one copy of their arrays; meshes without face-varying data flatten indexed (per point, not per corner); every mesh's arrays are freed once uploaded, uploads stage in 64 MB chunks, and a still subdivision surface frees its OpenSubdiv refiner. Peak in husk: Kitchen_set at husk's default complexity (level 3, 28M triangles) 11.4 to 4.3 GB, crowd 9k 12.6 to 7.6 GB, 25M unique triangles 6.3 to 4.8 GB. Culling (frustum, small objects, LOD) produces indirect draws, also for shadow cascades and atlas tiles. Draws split at 500M triangles to avoid driver timeouts. |
| Zero cost when off | Shading pipelines compile in sets per combination of optional features (`kCompiledFeatures`). A new combination compiles about 96 pipelines (up to 35 s cold on heavy scenes). |
| Pipeline cache | `eldr:pipelineCache`: `%LOCALAPPDATA%\Eldr\pipelines-<driver uuid>-<build key>.bin`; the key covers the shader hash and the size and time of the module holding all shaders (hdVk.dll, vkrtest.exe), so a new build starts a fresh file holding only its own pipelines (an old file kept stale entries: 332 MB against 86 MB fresh, about 2 s to load). Saving keeps the newest 3 other builds' files per driver. Pipelines the background prewarm compiles go into a build's file only on its first run (the driver appended them again on every run: 3 MB per run on Kitchen); the file then stays the same size. Crowd first frame 33.5 to 2.0 s. |
| Prewarm | `eldr:pipelinePrewarm` (Hydra default): after a feature change, compiles the sets one toggle away (GI, SSGI, fog first) on half the cores. |
| Culling | GPU frustum, small-object (`eldr:cullPixels`, sun shadows `eldr:shadowCullPixels`), LOD, distance light cut-off. No Hi-Z occlusion. |
| GPU sharing | With an OpenGL Hgi, colour and depth skip the CPU (1080p 17.4 to 3.2 ms) |
| Numerics | NaN/Inf cleaned where they enter each history (TAA, SSGI, GI, exposure) |

Costly opt-ins on a 1080 Ti at 1280x720: `eldr:ssrDenoise` +9 ms (crowd), `eldr:rtShadows` +10 ms (Kitchen), `eldr:rtReflections` +15 ms (Kitchen), `eldr:gi` 2-3 ms, volumes 7-22 ms for a large cloud. They matter little for seconds-per-frame renders.

## USD authoring conventions

All optional, beyond standard UsdPreviewSurface, UsdLux and UsdGeom.

Every Karma attribute Eldr reads has an `eldr:` twin; the order is the `eldr:` one when set, then Karma's when set, then the USD standard or default.

| Convention | On | Meaning |
|---|---|---|
| `bool primvars:eldr:reflectionProbe = 1` | mesh (typically an invisible Cube) | Reflection probe: never drawn, not in bounds or shadows; its world box is the influence and parallax volume, captured from its centre. Max 8, smallest first. Needs `eldr:reflectionProbe`. |
| `bool primvars:eldr:planarReflection = 1` | flat mesh | Planar mirror. Needs `eldr:planarReflections`. Without any, the largest flat glossy opaque mesh. |
| `bool primvars:eldr:hair = 0` | BasisCurves (constant) | Opt out of hair shading (cables, ropes). Unauthored or true: hair. |
| `bool primvars:eldr:ribbons` | BasisCurves | Overrides `eldr:curveRibbons` per curve |
| `bool primvars:eldr:sprites` | Points | Overrides `eldr:pointSprites` per prim |
| `float inputs:eldr:toonBands = 3`, `float inputs:eldr:toonSoftness = 0.05` | material surface shader | Toon bands per unit of light (0 = none), band edge width |
| `inputs:eldr:clearcoatColor` | UsdPreviewSurface shader | Coat tint (read as `coat_color`); constant or connected |
| `int primvars:eldr:refineLevel` | subdivision mesh | Its own refine level instead of the host's (usdview Complexity, Houdini Level of Detail); still at most `eldr:maxRefineLevel` |
| `int primvars:eldr:object:holdoutmode = 1`, Karma's `karma:object:holdoutmode` | gprim | Holdout: black, zero alpha, keeps depth, ids, shadows. Mode 2 acts as matte. |
| `string primvars:eldr:object:rendervisibility`, `karma:object:rendervisibility` | mesh, curves, points | Ray visibility: `*`, `-primary` (phantom: unseen, still shadows and reflects), `-shadow`, `primary\|shadow`, `""` (unrenderable). Diffuse, reflect and refract are one class in Eldr (ray-traced reflections, GI, refraction); screen-space effects see what the camera sees. |
| `bool primvars:eldr:object:mblur = 0`, `karma:object:mblur` | gprim | No motion blur of its own (transform, deformation); camera motion still blurs it |
| `int primvars:eldr:object:vblur`, `instance_vblur`, `karma:object:vblur`, `instance_vblur` | mesh, curves, points; PointInstancer; volume | Velocity Blur: 0 (default, as Karma) ignores `velocities`, 1 blurs by `velocities` (instances also `angularVelocities`), 2 adds `accelerations`. Off together with `eldr:deformationBlur` (gprims) or `eldr:sampledMotion` (instances). A volume above 0 blurs along its `vel` field (`eldr:volumeVelocityBlur`). |
| `float primvars:eldr:object:volumevblurscale`, `karma:object:volumevblurscale` | volume | Volume Velocity Blur Scale: multiplies the `vel` field's blur (default 1) |
| `int primvars:eldr:object:curve_style`, `point_style`, `karma:object:curve_style`, `point_style` | BasisCurves, Points | Render Curves As (0, 2 ribbons; 1 tubes; 3 the render setting) and Render Points As (0, 2 discs; 1 spheres). Oriented ribbons without normals and oriented discs face the camera. `eldr:ribbons`, `eldr:sprites` win. |
| `float primvars:eldr:object:width_scale`, `karma:object:width_scale` | BasisCurves, Points | Multiplies the widths |
| `int primvars:eldr:object:dicingdepthmax`, `karma:object:dicingdepthmax` | subdivision mesh | Caps the refine level (-1: no cap) |
| `int primvars:eldr:object:dicingdepthmin`, `karma:object:dicingdepthmin` | subdivision mesh | Lowest refine level (-1: none) |
| `float primvars:eldr:object:dicingquality`, `karma:object:dicingquality` | subdivision mesh | With `eldr:screenDicing`: about quality² polygons per pixel (default 0.5, as Karma: a polygon per 4 pixels); the mesh gets the uniform level its nearest face needs. Off-screen faces count at 0.1 of it (Karma's offscreen quality). |
| `float primvars:eldr:object:volumelimit`, `karma:object:volumelimit` | volume (also a render setting of that name) | Above 0, volumes scatter more than once: octaves fitted to Karma at that limit (the largest in the frame counts for all volumes) |
| `bool primvars:eldr:renderAsSubdivision` | polygon mesh (subdivisionScheme none) | Renders it as Catmull-Clark without changing the mesh, as Mesh Edit would; then `refineLevel` applies. Needs `eldr:subdivision`. |
| `bool inputs:eldr:light:renderlightgeo`, `inputs:karma:light:renderlightgeo` | light | Light geometry visible to the camera (false hides it) |
| `string inputs:eldr:light:lpetag`, `inputs:karma:light:lpetag` | light, dome | Light group (LPE tag): `lpe:C.*<L.'tag'>` holds its lights' direct light |
| `collection:matte` | active UsdRenderPass | Collection members are holdouts (usdview: "Set As Active Render Pass"; `usdrecord --renderPassPrimPath`, needs the pass's `renderSource`). Whole gprims only. |
| UsdRenderSettings, RenderProduct, RenderVar | `/Render/...` | EXR products with AOVs and Cryptomatte; `sourceType` `lpe` and `primvar` honoured |
| `eldr:<setting>` | active UsdRenderSettings | Sets that render setting |
| `lensDistortion:k1,k2,k3,k4,k5,k6,p1,p2,center,anaSq,asym,scale`, `lensDistortion:type` (`standard`, `fisheye`) | camera (custom) | Lens warp; `k3`-`k6`, `p1`, `p2` are OpenCV terms |
| `karma:camera:material:binding`, `karma:camera:use_lensshader`, `karma:camera:lensshader` | camera | Karma Physical Lens `opencv_k1..k6`, `opencv_p1`, `opencv_p2` become the same distortion |
| `primvars:skel:skinningMethod = "dualQuaternion"` | skinned mesh | Dual-quaternion skinning (default linear blend) |
| `inputs:angle` | DistantLight | Sun angular diameter for soft shadows |
| `inputs:texture:file` | RectLight, DiskLight, DomeLight | Image on the light (left edge at +X, top row at +Y, as Karma) or the dome's latlong |
| `shadow:enable` | lights | Lights without it cast no shadows |
| `collection:lightLink`, `collection:shadowLink` | lights (not domes) | Light linking: what it lights, what casts its shadows (default: everything) |
| `inputs:shaping:ies:file`, `shaping:ies:angleScale`, `shaping:ies:normalize` | sphere, rect, disk, cylinder lights | IES profile |
| `emissiveColor` (constant) | UsdPreviewSurface / standard_surface emission | Small bright emitters become lights |
| `fStop`, `focalLength`, `focusDistance`, `horizontalAperture`, `shutter:open`, `shutter:close` | UsdGeomCamera | Depth of field, motion blur |
| `exposure`, `exposure:iso`, `exposure:time`, `exposure:fStop`, `exposure:responsivity` | UsdGeomCamera | Image exposure, beauty and light AOVs (as Karma) |
| `wrapS/wrapT` | UsdUVTexture | repeat, mirror, clamp, black, useMetadata (default). MaterialX `image` `uaddressmode`/`vaddressmode`: periodic, mirror, clamp, constant. |
| `<UDIM>` asset paths | textures | Tiles 1001-1100 found by the resolver, also in usdz. A missing tile, or a missing file, reads the texture node's own value as Karma: UsdUVTexture `fallback`, MaterialX image `default` (black unless authored), not the shader input's |
| Subdivision tags, GeomSubsets, `displayColor` | meshes | As UsdGeom defines |
| `fieldName`, `fieldIndex` and a MaterialX `volume` or Karma `kma_volume` material | UsdVolVolume with OpenVDBAsset | Fields by the material's `geompropvalue` names; without a material `density` (`eldr:volumeDefaultFields`: also `temperature`, `Cd`/`color`, `emission`) |

## Known limitations

| Area | Limitation |
|---|---|
| Output | All render products use the first raster product's framing; the collection is ignored. Vars Eldr cannot fill are left out with a warning. No `elementId` AOV. |
| Output | Cryptomatte ranks come from film samples in the 3x3 filter footprint; thin objects between samples can be missed. Interactive: one id per pixel. |
| Hydra | Volumes are not pickable; a field prim's own transform is ignored; OpenVDB assets only. |
| Hydra | Dome `poleAxis`, portals and shadow colour/distance are ignored; domes and the fog ignore light linking; IES types A and B are read as C, TILT is ignored. One frame in flight. |
| Lights | 255 lights per cluster (a fuller cluster loops over all local lights: slower, not wrong); every cluster tests every light. |
| Lights | Textured lights use two taps, not a filtered polygon; traced hits, probes, fog and volumes read one tap above the shaded point (no highlight tint); emissive-as-light ignores the texture. |
| Lights | Emissive surfaces become lights only when constant, rigid, small and at most 32; others light via GI only. |
| Shadows | Sun: no blend between cascades; kernels clamp to 5% of a cascade. Local lights: redraw is all-or-nothing when a caster moves; kernels clamp at cube-face seams. |
| Fog | Single-tap shadows; the dome's light in fog is unoccluded without GI. |
| GI | Probes can't tell a single-sided wall from the inside of a closed object; meshes without normals use winding. |
| SSGI, SSR | No depth mip chain (large radii sample sparsely); SSR runs at full resolution with one ray and reflects only what is on screen. |
| Probes, planar | Probe reflections lack screen-space effects and volumes; CPU prefilter; no seam filtering. Planar: rough mirrors blur by a fixed disc (no contact hardening), refraction through planar water not done; coplanar mirrors don't share renders. |
| Ray tracing | Translucent meshes don't block rays (except tinting shadows and refraction); skinned instances capped; static BLASes compacted after building (`eldr:rtCompaction`; skinned and displaced ones refit uncompacted); local lights unshadowed at hit points; closest-hit normal on displaced meshes is the rest triangle's. |
| Transparency | OIT and sorting are per mesh (one order for refracting and other translucent meshes). Holdouts cut everything at the pixel, from constants or textures (not programs). Subsurface is a per-channel Gaussian (not Burley). |
| Refraction | Screen space unless `eldr:rtRefraction`. Traced refraction follows only the nearest refracting surface's ray; skinned glass beyond `eldr:rtSkinnedInstances` isn't traced. Volumes between translucent layers are dimmed by the layers in front of them but not refracted, and also dim those layers' reflections; layers past `eldr:refractionLayers` (at most 4) count as behind the last one. Under traced refraction volumes aren't refracted (they go over the glass, dimmed). |
| Materials | Unsupported graph nodes are bypassed (their pass-through input or default); Arnold or RenderMan volume shaders keep the constant. Volume graphs: Karma's Pyro Shader and Pyro Preview aren't read, nor blackbody of a ramp or remap, a colour field times a ramp, or more than three field functions. A field function that isn't 0 at 0 (a remap with `outlow` above 0, a ramp that starts above black) applies only where the density grid has data, not in its empty tiles; Karma's goes on to the volume's bounds. Catmull-Rom overshoot below 0 is clamped (Karma keeps negative emission). Volume shadows (maps, traced, volume on volume) are per channel from the material's coefficients or the density's ramps; a colour field (`Cd`) tinting the scattering doesn't colour them. Multiple scattering octaves don't reach local lights (volume_cubes.usda: about half of Karma's gain) and use one limit for all volumes. A constant coefficient (no field read) counts per unit of density; Karma fills the active voxels uniformly. Gray textures in gamma colour spaces aren't decoded. No tangent primvars (frame follows uv derivatives). |
| Materials | Coat: no coat normal, anisotropy or SSR; no `coat_darkening`. Thin film assumes air above. Hair has no TT lobe; a closed curve's u jumps at its weld. |
| Materials | With a textured setting off, a material with a textured weight is still drawn in the translucent or subsurface pass (weight 0). |
| Displacement | Top mip only; vector displacement drops tangential parts; skinning stretch doesn't scale bump; `displacementEnabled` not read. |
| Textures | Past the table size they fall back to constants. A mesh carries up to two extra uv sets; a GeomSubset with a different first uv set reads the mesh's. |
| LOD | Border and seam vertices never move (many uv islands simplify little); meshes with over a million instances keep one coarser level. |
| Curves | World-space 6-sided tubes (12 triangles per step) unless ribbons. |
| Culling | No Hi-Z occlusion culling. |
| Camera | Box shutter, linear motion between frames. Film filter gather is 3x3 (enough to 1.5 px). Overscan renders at output resolution (centre loses up to 2x detail). Karma's curvature and aberration lens effects are not matched. |
| Sampling | Adaptive sampling rarely stops early on scenes with edges. |
| Post | Bloom doesn't spill onto the app's background colour. |
| Tooling | Prewarm handles single toggles only and prewarms sets without light splits. Old cache files stay on disk. Scene upload is one synchronous submit per commit; the volume grid pool uploads whole on any change. The Attribute Editor (usdview plugin) edits the session layer only. |
