# Eldr render settings

Every setting is a Hydra render setting named `eldr:<name>`. A setting you don't set uses the default shown.

| Where | How |
|---|---|
| usdview | **Eldr > Render Settings**: grouped, filterable, changed settings in bold, one reset button |
| Python | `engine.SetRendererSetting("eldr:ssr", False)` |
| USD | an `eldr:<name>` attribute on the active UsdRenderSettings prim (typed like the default) |

**One set of settings everywhere.** There are no quality presets: the viewport, husk and usdrecord render the same image from the same settings. The defaults give a clean image quickly; when the camera stops, the film keeps adding samples until it converges. Offline renders (husk, usdrecord) only run faster (batched samples, fewer readbacks), never differently.

**Final renders:** turn on the expensive settings on your render settings prim. `tests/scenes/final_settings.usda`'s `/Render/final` is a ready-made one to reference: `eldr:filmRestart`, `eldr:rtReflections`, `eldr:rtRefraction`, `eldr:rtHitShadows`, `eldr:finalGather` and `eldr:caustics` on, `eldr:rtShadowSamples` 1, `eldr:rtShadowLights` 16. A production final also turns on `eldr:gi`, `eldr:rtShadows` and `eldr:denoise` and sets `eldr:filmSamples` to 256 or more. In Houdini's viewport, pick that render settings prim to see it (docs/houdini.md).

**Settings in old scenes:** a few settings changed type. Eldr still reads the old values:

| Setting | Old value | Read as |
|---|---|---|
| `eldr:rtReflections`, `eldr:rtRefraction`, `eldr:rtHitShadows`, `eldr:finalGather`, `eldr:caustics` | int 0, 1, 2 | on at 2 only |
| `eldr:textureCompression` | int 0, 1, 2 | on at 1 or 2 |
| `finalQuality`, `finalShadows`, `adaptiveFinalThreshold` | any | ignored (use the final render settings above, `eldr:rtShadows`, `eldr:rtShadowsAuto`, `eldr:adaptiveThreshold`) |

**Try a setting:** most features have a scene in `showcase/`, listed by topic in the guide's [Showcase scenes](guide/showcase.md) chapter. Open `showcase/<folder>/scene.hip` and compare `eldr_default`, `eldr_final` and `karma`, then the feature's A/B nodes.

## Reading the tables

| Mark | Meaning |
|---|---|
| flag, int, float, string | Type; values outside the range are clamped |
| [film] | Acts only while the film accumulates (camera still) |
| [interactive] | Acts only while the camera or scene moves |
| [RT] | Needs a GPU with ray tracing; ignored without one (`HDVK_NO_RAY_TRACING=1` pretends there is none) |
| [prepass] | Needs `eldr:depthPrepass` |
| Cost | GPU time, GTX 1080 Ti, 1280x720 unless stated. Crowd = 20,000 skinned agents (21 ms at defaults). Kitchen = Pixar's Kitchen_set. Under 1 ms is noise. "A to B" compares the two states the row describes. |

Shader code for a setting that is off is left out. The first frame that uses a new combination of settings compiles its shaders (seconds, unless cached on disk).

Groups follow topics. The switches people change most come first, in open groups (Sampling, Ray Tracing, Global Illumination, Camera, Image, Volumes and Fog, Viewport). Their fine controls follow under Advanced, with the same topics, folded in the UIs. AOVs and Products and Performance and Debug come last. The order matches `hydra/renderSettings.def`, where every setting is defined.

**Terms used below**

| Term | Meaning |
|---|---|
| Film | Averaging of many samples (jittered renders) while the camera is still, until `eldr:filmSamples`. |
| TAA | Temporal anti-aliasing: blends each frame with the previous ones while things move. |
| AOV | An extra image output (render var), such as normals, depth or a light group. |
| GI | Global illumination: light bouncing off surfaces. Eldr stores it in a grid of probes (DDGI). |
| Final gather | One traced ray per pixel and sample that collects bounced light directly. |
| SSR, SSGI | Screen-space reflections and GI: rays tested against the depth image, so off-screen things are missing. |
| AO (GTAO) | Ambient occlusion: darkening in corners and contact areas. |
| Shadow maps, PCF, PCSS | Shadows from depth images rendered from the light. PCF softens edges by a fixed amount; PCSS softens them by the light's size and the distance to the blocker. |
| BLAS | The ray-tracing structure of one mesh. |
| OIT | Order-independent transparency: blends transparent layers without sorting them. |
| Froxel | A cell of the fog grid, aligned with the camera's view. |
| Firefly | A single very bright pixel from a rare light path. |
| RMSE, PSNR | Error against a reference image: lower RMSE and higher PSNR (dB) mean closer. |

## Sampling

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Film Samples<br>`eldr:filmSamples` | int 64 | [film] Samples per pixel until the image is converged, in every host. Use 256 or more for noise-free finals. | Linear |
| Film Restarts at Once<br>`eldr:filmRestart` | off | [film] Any change restarts the film from its first sample at once. Off: TAA while things move, and the film starts after a few still frames. | Interactive: no TAA upscaling, a fresh film per change |
| Denoise<br>`eldr:denoise` | off | [film] Cleans the converged image with Intel Open Image Denoise on the CPU, guided by albedo and normals. Can smear fine detail. The eldr:beautyRaw AOV keeps the undenoised image. Karma's `karma:hydra:denoise` also turns it on. | 1080p: about 1.3 s once per frame (CPU); guide pass +10 ms per sample |
| Diffuse Quality<br>`eldr:diffuseQuality` | float -1 (unset: Karma's, else 1) | Karma's `karma:object:diffusequality`: multiplies the rays used for bounced diffuse light (final gather, GI probes, AO, SSGI). Values below 1 skip some samples. | emissive.usda 960x540: 15 / 20 / 35 ms per sample at 0.5 / 1 / 2; 2 at 32 samples is as clean as 64 |
| Reflection Quality<br>`eldr:reflectQuality` | float -1 (unset: Karma's, else 1) | Karma's `karma:object:reflectquality`: multiplies the traced reflection rays. Fractions spread over film samples. | Linear in the reflections |
| Refraction Quality<br>`eldr:refractQuality` | float -1 (unset: Karma's, else 1) | Karma's `karma:object:refractquality`: multiplies the ray-traced refraction paths through rough glass (fractions spread over film samples) and the screen-space refraction blur taps (up to 16). | glass.usda 960x540, traced: 19.9 / 22.5 / 33.1 ms per sample at 1 / 2 / 4; 2 at 16 samples is as clean as 1 at 32. Screen space: 4.9 / 5.2 / 5.4 ms |
| Light Quality<br>`eldr:lightQuality` | float -1 (unset: Karma's, else 1) | Karma's `karma:global:light_sampling_quality`: multiplies the traced shadow rays per light. Shadow maps don't change. | Linear in the traced shadows |
| Subsurface Quality<br>`eldr:sssQuality` | float -1 (unset: Karma's, else 1) | Karma's `karma:object:sssquality`: multiplies `eldr:subsurfaceSamples`. | Linear in the subsurface blur |
| Volume Quality<br>`eldr:volumeQuality` | float -1 (unset: Karma's, else 1) | Karma's `karma:object:volumequality`: multiplies `eldr:volumeShadowSteps`. | Linear in the volume lighting |

## Ray Tracing

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Ray-Traced Shadows<br>`eldr:rtShadows` | off | [RT][prepass] Traced shadows for the sun and the most important local lights (`eldr:rtShadowLights`), instead of shadow maps. Softness comes from the light's size; the film averages the noise. | Kitchen about 10 ms |
| Ray-Traced Shadows when Cheaper<br>`eldr:rtShadowsAuto` | off | [RT] When `eldr:rtShadows` is off, traces shadows instead of using maps where that is likely cheaper: dense scenes (2+ triangles per pixel) whose skinned instances are all ray traced. | 960x540, 64 samples, traced / maps: City 2.2 / 4.5 s; crowd 0.8 / 1.4 s; Kitchen 0.93 / 0.64 s |
| Ray-Traced Reflections<br>`eldr:rtReflections` | off | [RT][prepass] Traced reflections up to roughness 0.7, replacing dome and screen-space reflections. They show off-screen objects. | Kitchen about 15 ms |
| Ray-Traced Refraction<br>`eldr:rtRefraction` | off | [RT][prepass] Traces rays through glass: back faces, glass behind glass, total internal reflection and absorption, with off-screen objects visible. Rough glass blurs over film samples. | glass.usda 8.1-9.7 ms; crowd unchanged |
| Ray-Traced Hit Shadows<br>`eldr:rtHitShadows` | off | [RT] Objects seen in traced reflections, refraction and final gather get shadows from all distant lights and up to 16 local lights. Off: only the sun's, so shadowed floors reflect as lit. | Up to one ray per light per hit |
| Final Gather<br>`eldr:finalGather` | off | [RT][prepass] With GI on, traces one ray per pixel and sample for bounced diffuse light, replacing probes, AO and SSGI there. Gives contact shadows, dark corners and colour bleeding. | 1080p, 64 samples: emissive.usda +0.34 s, Cornell +0.15 s per frame; interactive +5.5 ms per frame |
| Caustics<br>`eldr:caustics` | off | [RT] Light focused through glass or off mirrors onto diffuse surfaces (photon mapping), from sun, sphere, rect and disk lights. Only objects with Karma's Enable Caustics primvar cast them. Sharper every sample; AOV `caustics`. | caustics.usda 960x540: 5.9 ms per sample off, 13.3 ms on (65536 photons) |

## Global Illumination

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Global Illumination<br>`eldr:gi` | off | [RT] Bounced diffuse light from a grid of ray-traced probes. Use it for interiors and scenes lit by emissive surfaces. | Kitchen 2-3 ms (1728 probes x 128 rays); crowd 0.2 ms; plus ray-tracing memory |
| Screen-Space GI<br>`eldr:ssgi` | off | [prepass] Short-range bounced light and colour bleeding gathered from the visible image. Needs no ray tracing; replaces AO. | Half res 0.67 ms, full 1.55 ms (Kitchen) |
| Ambient Occlusion<br>`eldr:ambientOcclusion` | on | Darkens the dome's light in corners and contact areas. [prepass] SSGI replaces it when on. | 0.34 ms |
| Dome Lighting<br>`eldr:imageBasedLighting` | on | Lights the scene with the dome light's image (diffuse and reflections). Off: direct lights only. Also needed by the reflection probe. | Small |
| Dome Sun Extraction<br>`eldr:domeSun` | on | Turns the sun in an HDRI dome into a distant light that casts shadows. Off: the dome image only. | CPU work when the dome changes |

## Camera

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Motion Blur<br>`eldr:motionBlur` | on | Blurs moving things while the camera's shutter is open (`shutter:open`, `shutter:close`). While moving, a fast post-process blur. | Post blur 0.2 ms |
| Motion Blur Time Samples<br>`eldr:motionBlurTimeSamples` | on | [film] Each film sample renders the whole scene at one time in the shutter, so shadows, reflections and refraction blur correctly. Off: post-process blur. | Crowd 1.7-2.1 ms per sample |
| Depth of Field<br>`eldr:depthOfField` | on | Lens blur from the camera's `fStop`, `focalLength` and `focusDistance`. Perspective cameras only; not usdview's free camera. | Post DOF about 1.9 ms at 1080p |
| Camera Exposure<br>`eldr:cameraExposure` | on | The USD camera's exposure (`exposure`, `exposure:iso`, `:time`, `:fStop`, `:responsivity`) scales the beauty and light AOVs, as Karma; data AOVs stay. EV on top of the exposure compensation; colour limits apply to exposed values. | |

## Image

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Exposure Compensation<br>`eldr:exposureCompensation` | float 0 | EV added, always (also with Linear Beauty on); the fixed exposure when auto exposure is off. | |
| Linear Beauty<br>`eldr:productLinear` | on | Houdini's viewport and husk get linear scene colour: no tonemapper, no bloom, no auto exposure unless the settings prim sets it. Off: Eldr's tonemapped image. | One extra pass |
| Auto Exposure<br>`eldr:autoExposure` | off | Adjusts exposure over time from the image's brightness. Off: fixed exposure from `eldr:exposureCompensation`, as Karma. | 0.11 ms |
| Tonemapper<br>`eldr:tonemapper` | string aces | `aces`, `agx`, `agxPunchy` or `none` (exposed linear, unclamped). Ignored while Linear Beauty is on (Houdini viewport, husk products): none there. | |
| Bloom Intensity<br>`eldr:bloomIntensity` | float 0.04 | 0-1, share of the blurred image mixed in. Ignored while Linear Beauty is on (Houdini viewport, husk products). | |

## Volumes and Fog

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Volumetric Fog<br>`eldr:fog` | off | Height fog lit by all lights, with light shafts. Set it up under Fog and volumes. | 0.19 ms on crowd; shading +0.5 ms |
| Volumes<br>`eldr:volumes` | on | Renders OpenVDB volumes (smoke, clouds, fire). Needs a USD build with OpenVDB. | See Fog and volumes |
| Volume Multiple Scattering<br>`eldr:volumeMultipleScattering` | off | Light bounces more than once inside volumes, brightening thick clouds, like Karma's Volume Limit 16. A Volume Limit above 0 (`eldr:` or `karma:object:volumelimit`) sets the amount per volume. Off: one bounce, Karma's default. | Within 9% of Karma at limits 1, 4, 16 (backlit sun-only up to 23% too bright) |

## Viewport

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Film when Still<br>`eldr:filmAccumulation` | on | After 3 still frames, switches from TAA to the film: an exact average of samples. Off: TAA only, also when still. | 0.23 ms |
| TAA Samples<br>`eldr:taaSamples` | int 16 | Jitter cycle length; frames a still camera averages without the film; AOV averaging limit. | |
| Still Delay<br>`eldr:stillDelay` | float 0.3 | [interactive] Seconds (0 to 30) a stopped camera still counts as moving, so short pauses don't jump to full quality and back. 0: the first still frame is full quality. Not used offline. | crowd 9k / Kitchen / emissive RT: see docs/guide/viewport.md |
| Render Scale<br>`eldr:renderScale` | float 1 | [interactive] Below 1 (min 0.25), renders smaller while moving and TAA upscales. The film and extra AOVs stay full size. | Crowd moving: 27.0 ms at 1.0, 23.0 at 0.75, 16.4 at 0.5 |
| Render Scale while Moving<br>`eldr:movingRenderScale` | float 0.5 | [interactive] While the camera or scene moves, renders at most at this scale, or starts from it when a frame budget is set. 1: off. | Moving, 1920x1080: Kitchen 21.6 to 8.6 ms, crowd_9k 37.4 to 26.7, emissive RT 64.2 to 12.5 (with the two below) |
| Interactive Noise Loop<br>`eldr:interactiveSamples` | int 8 | [interactive] Sample dimensions loop over this many samples (1-256) so TAA settles. | |
| Ray-Traced Shadow Rays while Moving<br>`eldr:movingShadowRays` | int 1 | [interactive][RT] Traced shadow rays per light while moving; TAA smooths them. 0: as many as when still. | emissive 1080p moving: 11.7 to 5.1 ms (4 to 1 rays) |
| Pause Settled GI Probes while the Camera Moves<br>`eldr:movingGiPause` | on | [interactive][RT] While only the camera moves, settled GI probes stop updating, since their light doesn't depend on the camera. | emissive giProbes 4.0 ms to 0 while moving |
| Ramp to Full Quality<br>`eldr:rampFrames` | int 6 | [interactive] Frames over which quality steps back up after moving (render scale, shadow rays, GI probes) before the film starts. 0 or 1: at once. Not used offline. | Houdini 1866x737, emissive RT: worst frame after the stop 41.7 to 31.7 ms; full quality 0.33 to 0.47 s after the stop. Each step resizes the targets (about 3 ms CPU) |
| Frame Budget while Moving<br>`eldr:movingFrameBudget` | float 33 | [interactive] Target GPU milliseconds per frame while moving: the render scale adapts (0.25-1), so light scenes stay full size and heavy ones shrink. 0: use Render Scale while Moving. Not used offline. | Moving, 1866x737: crowd_9k RT 80 to 36 ms (quarter size), Kitchen 9.4 to 17.6 ms (full size), emissive RT 11.5 to 27.6 ms |
| Film Warm Start<br>`eldr:filmWarmStart` | float 4 | [interactive] The film starts from the last TAA image, counted as this many samples, so its first frames are already clean. 0: a clean start. Film Restarts at Once always starts clean. | None |

## Advanced

Quality controls (steps, radii, resolutions, thresholds) and the remaining on/off switches.

### Sampling

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Sample Dimensions<br>`eldr:sampleDimensions` | on | Gives each random effect its own well-spread sample pattern, so noise has no visible patterns and converges faster. | None |
| Adaptive Sampling Noise Threshold<br>`eldr:adaptiveThreshold` | float 0 (off) | [film] Pixels whose noise falls under this stop sampling early. Try 0.002 for large flat areas. Not used with motion blur time samples. | Off is bit-identical |
| Adaptive Sampling Converged Pixel Fraction<br>`eldr:adaptivePixelFraction` | float 0.999 | [film] The frame is done once this fraction of pixels has stopped. 1 waits for all. | Cornell at 0.01: 27 samples at 0.9, 58 at 0.95 |
| Adaptive Sampling Min Samples<br>`eldr:adaptiveMinSamples` | int 16 | [film] Samples every pixel takes before it may stop. | |
| Adaptive Sampling Skips Stopped Tiles<br>`eldr:adaptiveTiles` | on | [film] Blocks of pixels that have all stopped skip their work, so later samples are cheaper. The image is the same. Not used where refraction, subsurface or post depth of field is in view. | Kitchen 1080p, threshold 0.01: 17.4 to 12.8 s |
| Effect Denoise Fade<br>`eldr:effectDenoiseSamples` | int 64 | [film] The final gather, traced shadow and reflection blurs fade out over this many samples, so the converged image stays sharp. 0: blur every sample (slightly soft). Off with `eldr:denoise`. | At 4/16/64 samples, RMSE raw vs blurred: emissive.usda 0.027/0.023/0.012 vs 0.009/0.008/0.008 |
| Direct Light Clamp<br>`eldr:directClamp` | float 0 (off) | Caps a pixel's direct light brightness, keeping hue. Firefly killer; costs energy. | |
| Indirect Light Clamp<br>`eldr:indirectClamp` | float -1 (unset: 20) | Karma's indirect colour limit (0 = off): caps bounce light (SSGI, SSR, traced hits, GI rays), the emission they see, and emissive-mesh lights (radiance x albedo, as Karma). Wins over `eldr:colorLimit` and Karma's. Not the dome. | |
| Color Limit<br>`eldr:colorLimit` | float -1 (unset: Karma's, else 20) | Karma's `karma:global:colorlimit` (this wins): caps emission the camera sees, hue kept (40 shows as 20, as Karma), and the indirect clamp unless `eldr:colorLimitShared` is off. Full HDR: 0, with `eldr:indirectClamp` 20. Karma's 0 is black (ignored): use 1e6. | |
| Indirect Clamp Shared with Color Limit<br>`eldr:colorLimitShared` | int -1 (unset) | 0 off, 1 on. Off: the indirect clamp comes from `karma:global:colorlimitindirect` rather than the color limit. Without it set, Karma's `colorlimitindlinked` decides. | |

### Ray Tracing

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Ray-Traced Light Sampling<br>`eldr:rtLightSampling` | on | [RT] Traced rays sample one light at a time, chosen by brightness, and skip lights that can't reach a pixel. Same converged image with many lights, faster. | 1080p, ms per sample: emissive 147 to 78, Kitchen 195 to 158, crowd 542 to 478; noise unchanged |
| Ray-Traced Skinned Instances<br>`eldr:rtSkinnedInstances` | int 16 | [RT] How many of the largest skinned characters (0-256) block rays in ray-traced effects. | Kitchen 0.45 ms (16), 0.69 ms (90); crowd 0.68 ms (16), 2.7 ms (256) |
| Ray Tracing Sees Displacement<br>`eldr:rtDisplacement` | on | [RT] Ray-traced effects see displaced surfaces. | Rebuilds when the displacement changes |
| Ray Tracing Compacts Static BLASes<br>`eldr:rtCompaction` | on | [RT] Shrinks the ray-tracing data of static meshes after it is built. Off: about twice the memory. | Applies to data built after the change |
| Final Gather Distance<br>`eldr:finalGatherDistance` | float 0 | 0: final gather rays travel until they hit. Above 0: their length in scene units, after which the GI probes supply the light. Faster but less exact. | emissive.usda 960x540, 256 samples: 5.2 s at 0, 3.9 s at 0.5 |
| Final Gather Rays per Pixel<br>`eldr:finalGatherRays` | int 1 | Gather rays per pixel and sample (1-16). With Final Gather Denoise, more than 1 gains little. | 1080p, 64 samples: emissive.usda 2.7 s at 1, 5.3 s at 2, 10.4 s at 4 |
| Final Gather Denoise<br>`eldr:finalGatherDenoise` | float 1 | Strength (0-1) of an edge-aware blur on the gathered light during the first `eldr:effectDenoiseSamples` samples. Textures stay sharp. 0: raw. | About 1 ms per sample at 1080p while active |
| Final Gather Roulette<br>`eldr:finalGatherRoulette` | off | [RT] From the second sample, each 8x8 block gathers on every other sample, weighted to match. Same converged image, but about 1.5x the noise at 64 samples. | 1080p, ms per sample: emissive 78 to 64, Kitchen 158 to 132, crowd 478 to 388 |
| Caustic Photons per Light and Sample<br>`eldr:causticsPhotons` | int 65536 | Photons each light sends towards caustic casters per sample (1024 to 4M). More means less noise per sample. | 7.4 ms per sample at 65536, 28 ms at 262144 (one sun) |
| Caustic Gather Radius<br>`eldr:causticsRadius` | float 0 (auto) | World units the first sample gathers photons over; later samples shrink it (progressive photon mapping, alpha 2/3). 0: 2% of the casters' bounding radius. Larger is smoother but blurrier at first. | |
| Ray-Traced Refraction Interfaces<br>`eldr:rtRefractionBounces` | int 4 | Surfaces a ray passes (1-16): 2 for one ball, 4 for a ball behind a pane. | |
| Ray-Traced Refraction Sees Displacement<br>`eldr:rtRefractionDisplacement` | on | [RT] Displaced glass bends rays by its displaced shape (with `eldr:rtDisplacement`). Off: it refracts as flat glass. | Displaced glass only |
| Ray-Traced Refraction Starts at Glass<br>`eldr:rtRefractionClip` | on | [RT] Traces refraction only from pixels that show glass. The image is the same; off is for comparison. | glass.usda 1080p: 41.7 to 37.5 ms per sample |

### Global Illumination

| Setting | Default | What it does | Cost |
|---|---|---|---|
| GI Probes along the Scene<br>`eldr:giProbes` | int 16 | Probes along the scene's longest side (2-64, at most 16384 in all). More catch smaller detail. | Linear in probes |
| GI Rays per Probe per Frame<br>`eldr:giRaysPerProbe` | int 128 | Rays each probe traces per frame (16-512). More converges faster and cleaner. | Linear |
| GI Hysteresis<br>`eldr:giHysteresis` | float 0.97 | History weight (0-0.99). Higher is smoother, slower to react. Little effect on stills. | |
| GI Preconverge<br>`eldr:giPreconverge` | on | [film] Waits for fresh probes to settle before the film starts averaging. | Cornell: RMSE 0.0128 to 0.0047 |
| GI Probe Relocation<br>`eldr:giRelocation` | on | Moves probes that sit inside or just behind walls out into open space, so light near walls stays right. | emissive.usda (4096 probes) 2.50 to 3.04 ms |
| Ambient Occlusion Radius<br>`eldr:aoRadius` | float 0.5 | World-space scan radius. Match scene scale. | |
| Ambient Occlusion Directions<br>`eldr:aoSlices` | int 3 | Directions per pixel (1-8). 3 is enough with the film. | Linear |
| SSGI Steps<br>`eldr:ssgiSteps` | int 8 | Steps per direction and side (1-32). | |
| SSGI Radius<br>`eldr:ssgiRadius` | float 1 | World-space gather radius; use a fraction of the room size. | |
| SSGI Resolution Divisor<br>`eldr:ssgiResolution` | int 2 | 1 = full resolution, 2 = half (denoised and upsampled). | Full is about 2x |
| SSGI Tile Order<br>`eldr:ssgiTileOrder` | on | At half resolution, traces a different pixel of each 2x2 block every frame. Off: always the same one. | Free |
| SSGI Visibility Bitmask<br>`eldr:ssgiBitmask` | off | Lets light pass behind thin objects. Use it for poles, railings and foliage. | Cornell 0.20 vs 0.14 ms |
| SSGI Bitmask Object Thickness<br>`eldr:ssgiBitmaskThickness` | float 0.25 | Assumed object thickness, as a fraction of `eldr:ssgiRadius`; thicker blocks more light. | Free |
| Screen-Space Thickness Estimator<br>`eldr:ssgiThickness` | on | Lets SSGI and SSR rays pass behind thin objects and handle grazing surfaces. Off: every surface counts as a fixed thickness. | Noise |
| Screen-Space Back-Face Hits<br>`eldr:screenBackface` | on | An SSR or SSGI ray that hits the back of a surface keeps only `eldr:screenBackfaceScale` of its light, so less light leaks through objects. | SSR 0.27 ms on crowd; SSGI 0.60 to 0.76 ms |
| Screen-Space Back-Face Hit Scale<br>`eldr:screenBackfaceScale` | float 0.25 | Light kept from a back-face hit (0-1): 0 treats surfaces as one-sided, 1 as two-sided. | Free |

### Lights

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Local Lights<br>`eldr:localLights` | on | Sphere, rect, disk, cylinder lights. | |
| Clustered Local Lights<br>`eldr:clusteredLights` | on | 16x9x24 cluster grid, up to 255 lights per cluster (more: that cluster shades all lights). Off loops all lights per pixel. | |
| Light Textures<br>`eldr:lightTextures` | on | Rect and disk lights multiply radiance by `inputs:texture:file` (Karma orientation), in the light they cast and on their visible shape. | |
| Light Linking<br>`eldr:lightLinking` | on | Honours light and shadow links (`collection:lightLink`, `collection:shadowLink`, Houdini's Light Linker), up to 32 linked collections. Domes and fog ignore links. Off: every light lights and shadows everything. | Shadow-linked lights trace slower shadow rays |
| IES Profiles<br>`eldr:lightIes` | on | Shapes local lights with an IES file (`inputs:shaping:ies:file`, with `angleScale` and `normalize`), oriented as in Karma. | One texture read per light and pixel |
| Light Shapes Visible<br>`eldr:lightShapesVisible` | on | Shows light shapes to the camera. A light shows its shape only when its Render Light Geometry (`inputs:karma:light:renderlightgeo` or `inputs:eldr:light:renderlightgeo`) is on, as in Karma. Needs `eldr:localLights`. | 0.12 ms |
| Dome in the Camera<br>`eldr:domeCameraVisible` | on | Draws the dome's texture behind the scene. Off: the clear colour (with its alpha) shows; lighting and reflections still use the dome. `bool inputs:karma:light:renderlightgeo = 0` hides one dome; Hydra's `domeLightCameraVisibility` also applies. | |
| Emissive Surfaces as Lights<br>`eldr:emissiveLights` | on | Small bright constant emitters also become rect or sphere lights with shadows (max 32). Others light via GI only. | |
| Emissive Light Threshold<br>`eldr:emissiveLightThreshold` | float 4 | Brightest-channel radiance above which an emitter becomes a light. | |
| Dome Sun Threshold<br>`eldr:domeSunThreshold` | float 10 | Luminance counting as sun. Lower if the sun is missed. | |
| Direct Light Intensity<br>`eldr:directIntensity` | float 1 | Scales direct light in the final image only; bounced light and AOVs are unchanged. | None at 1. Other values add the light-split pass: crowd shading 14.8-16.5 vs 12.2-12.5 ms |
| Indirect Light Intensity<br>`eldr:indirectIntensity` | float 1 | Scales bounced light and reflections in the final image only. 0 leaves direct light only. | As above |

### Shadows

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Sun Shadows<br>`eldr:shadows` | on | Cascaded shadow maps for the first shadow-casting distant light. | |
| Sun Shadow Resolution<br>`eldr:shadowResolution` | int 2048 | Pixels per side of each cascade's map (256-8192). | Grows with the square |
| Sun Shadow Cascades<br>`eldr:shadowCascades` | int 4 | Number of cascades (1-4). Fewer is faster but blurrier. | 1 cascade saves about 12 ms on crowd |
| Further Sun Shadow Maps<br>`eldr:extraSunShadowMaps` | on | Up to 2 distant lights after the first, such as a dome's extracted sun, get one hard-edged shadow map each over the view; surfaces, glass, volumes and fog read it. Further ones stay unshadowed. Off: they cast none unless traced. | One shadow view per further sun, drawn once while the scene stands still; the 64 MB atlas if no local light has one. crowd_9k 2048x1080, 64 samples: 18 s either way, +60 MB |
| Local Light Shadows<br>`eldr:localShadows` | on | Shadow maps for sphere, rect, disk and spot lights, nearest lights first; lights that don't fit stay unshadowed. | Cached while still |
| Local Light Shadow Resolution<br>`eldr:localShadowResolution` | int 512 | Texels per tile (64-2048). Tiles halve (to 64) so every shadowed light fits. | |
| Soft Shadows<br>`eldr:softShadows` | on | Contact-hardening penumbrae for local lights from their size. Off: 3x3 PCF. | |
| Soft Sun Shadows<br>`eldr:softSunShadows` | on | The same for the sun, from its `inputs:angle`. | Crowd 1.27 ms |
| Soft Shadow Samples<br>`eldr:softShadowSamples` | int 16 | Samples per soft shadow lookup (1-16). | Linear |
| Jittered Light Shape per Sample<br>`eldr:shadowJitter` | on | [film] Each film sample renders shadow maps from a different point on the light, so soft shadows converge to the exact result. Needs soft shadows. | Sun error 0.016 vs 0.065 without; no frame-time change |
| Translucent Shadows<br>`eldr:translucentShadows` | on | Glass, water and tinted objects cast coloured shadows, also into fog. Volumes cast shadows the same way. | 0.04 ms; 32 MB per map at default sun size |
| Translucent Shadow Resolution Divisor<br>`eldr:translucentShadowDivisor` | int 2 | Tint map size as 1/N of the shadow maps (1, 2, 4). Also sets volume shadow resolution. | |
| Ray-Traced Shadow Rays per Light<br>`eldr:rtShadowSamples` | int 4 | Shadow rays per light, pixel and sample (1-16). 1 is enough for the film, which averages them. | Linear |
| Ray-Traced Shadow Lights<br>`eldr:rtShadowLights` | int 4 | [RT] How many lights `eldr:rtShadows` traces: the sun and the most important local lights (1-16). The rest use shadow maps. | Up to a ray per light and pixel |
| Ray-Traced Shadow Denoise<br>`eldr:rtShadowDenoise` | on | [RT] Blurs traced shadows across their soft edge only, so hard contact shadows stay hard. Fades out over `eldr:effectDenoiseSamples`. Off: raw rays. | One extra ray per light on shadowed pixels while active; no bias on the Kitchen |
| Ray-Traced Shadows through Glass and Volumes<br>`eldr:rtTranslucentShadows` | on | [RT] Traced shadows pass through glass (taking its colour) and through volumes. Off: those objects cast no traced shadow. | Glass 1.08 vs 1.01 ms; volume 0.62 vs 0.49 ms |
| Ray-Traced Shadows of Further Suns<br>`eldr:rtExtraSunShadows` | on | [RT] While ray tracing runs for another effect, distant lights after the first, such as a dome's extracted sun, cast traced shadows. Never starts ray tracing itself. Off: their shadow maps (`eldr:extraSunShadowMaps`). | volume.usda 720p: 3.4 ms on a GTX 1080 Ti |

### Reflections

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Screen-Space Reflections<br>`eldr:ssr` | on | Reflections of what is visible on screen. [prepass] Needs post processing. | 6 ms on crowd |
| SSR Ray Steps<br>`eldr:ssrSteps` | int 32 | Steps per reflection ray (1-256). Raise it if long reflections break up. | Crowd shading 10.8 ms at 8, 11.7 ms at 32 |
| Denoised Glossy SSR<br>`eldr:ssrDenoise` | off | Blurry reflections on rough surfaces up to roughness 0.8, filtered to remove noise. Off: sharp reflections that fade out with roughness. Not used with ray-traced reflections. | Crowd shading 11.7 to 20.4 ms |
| SSR Hit Footprint Filter<br>`eldr:ssrHitFilter` | on | Rough reflections blur instead of sparkle, and fade out at the image edges. | 0.05 ms |
| Ray-Traced Reflection Rays per Pixel<br>`eldr:rtReflectionSamples` | int 1 | Reflection rays per pixel and sample (1-16). 1 is enough with the film. | Linear |
| Ray-Traced Reflection Denoise<br>`eldr:rtReflectionDenoise` | on | [RT] Blurs rough traced reflections slightly, fading out over `eldr:effectDenoiseSamples`. Off: raw rays. | About free |
| Ray-Traced Reflection Roulette<br>`eldr:rtReflectionRoulette` | on | [RT] Skips reflection rays where they add little: none on surfaces with no specular, fewer on dim reflections, weighted so the converged image is the same. | 1080p final: crowd 249 to 146 ms per sample, Kitchen 49 to 34 |
| Reflection Probe<br>`eldr:reflectionProbe` | off | Captures the scene once into a cube map and uses it for reflections instead of the dome. Good for interiors. Recaptured when the scene changes. Needs `eldr:imageBasedLighting`. | Capture: Kitchen 6.5 ms, crowd 122 ms; then about 1 ms |
| Reflection Probe Resolution<br>`eldr:probeResolution` | int 128 | Size of each cube face (16-512, power of two). | CPU about 10 ms at 128 |
| Reflection Probe Position<br>`eldr:probePosition` | string empty | "x y z" capture point; empty = scene bounds centre. | |
| Authored Reflection Probes<br>`eldr:authoredProbes` | on | Meshes with `primvars:eldr:reflectionProbe` are probes (max 8, smallest first). Needs `eldr:reflectionProbe`. | |
| Normalize Reflection Probes<br>`eldr:probeNormalize` | on | Scales a probe by local ambient light over captured ambient (GI or SSGI on), clamped 4x. | |
| Planar Reflections<br>`eldr:planarReflections` | off | Flat glossy surfaces (`primvars:eldr:planarReflection`, else the largest flat glossy one) reflect a mirrored render of the scene. | One extra render per mirror in view |
| Planar Reflection Scale<br>`eldr:planarScale` | float 0.5 | Mirror render size as a fraction of the frame (0.1-1). | |
| Planar Reflection Mirrors<br>`eldr:planarCount` | int 4 | Mirrors per frame, largest first (up to 4). | One render each |
| Planar Reflection Blur<br>`eldr:planarBlur` | float 1 | How much rough mirrors blur their reflection; 0 = always sharp. | 12 taps on mirror pixels |
| Planar Reflections at Time Samples<br>`eldr:planarMotionBlur` | on | [film] Mirrors render at each motion blur time sample, so reflections blur like the scene. Off: sharp reflections. | None |

### Materials

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Karma Default Material<br>`eldr:defaultMaterialKarma` | on | Prims without a material shade as in Karma: its default material over `displayColor`, with `displayOpacity` as opacity. Off: the base material's defaults over `displayColor`, opaque, as Storm. | |
| Normal Maps<br>`eldr:normalMaps` | on | Applies normal maps. | |
| Specular Workflow<br>`eldr:specularWorkflow` | on | UsdPreviewSurface `useSpecularWorkflow = 1` uses F0 = `specularColor`. Off: 0.04. | |
| Clearcoat<br>`eldr:clearcoat` | on | A glossy coat layer: UsdPreviewSurface `clearcoat`, standard_surface `coat`, OpenPBR `coat_*`. | Only materials that use it pay |
| Sheen<br>`eldr:sheen` | on | A soft velvet-like layer: standard_surface `sheen`, OpenPBR `fuzz_*`. | Same |
| Thin Film Iridescence<br>`eldr:thinFilm` | on | Soap-bubble colours: standard_surface `thin_film_thickness`, OpenPBR `thin_film_*`. | Same |
| Anisotropic Specular<br>`eldr:anisotropicSpecular` | on | Stretched highlights, as on brushed metal (`specular_anisotropy`, `specular_rotation`, OpenPBR `specular_roughness_anisotropy`). | Same |
| Multiple Scattering<br>`eldr:multiScatter` | on | GGX energy compensation (MaterialX's) where the surface's spec has it: standard_surface and OpenPBR. UsdPreviewSurface is single scattering, as Storm and Karma. Off: single scattering everywhere. | |
| Hair Shading<br>`eldr:hairShading` | on | BasisCurves shade as hair (Kajiya-Kay), no SSR. `primvars:eldr:hair = 0` opts a prim out. | |
| Toon Shading<br>`eldr:toon` | on | Materials with `inputs:eldr:toonBands` quantise lighting into flat bands. | |
| Toon Bands for Every Material<br>`eldr:toonBands` | int 0 (off) | Bands per unit of light for all materials. Whole-scene NPR look. | |
| Textured Lobe Inputs<br>`eldr:texturedLobes` | on | Coat, sheen, anisotropy, thin film, specular and IOR inputs can be textures or shading networks. Off: constant values only. | Only such materials |
| MaterialX Graphs<br>`eldr:materialGraphs` | on | Runs MaterialX and UsdShade shading networks behind material inputs. Off: plain values and textures only. | Full screen about 2.5 ms at 1080p; Houdini's CarPaintMetallic 38 ms |
| Procedural Textures<br>`eldr:proceduralTextures` | on | MaterialX noise nodes. Off: noise reads 0. | fractal3d 0.74 ms, worleynoise3d 0.5 ms (full screen) |
| Subsurface Scattering<br>`eldr:subsurface` | on | [prepass] Light spreading under the skin of skin, wax or marble, plus sun shining through thin parts. | About 0.3 ms, only with subsurface materials |
| Subsurface Samples<br>`eldr:subsurfaceSamples` | int 15 | Taps per direction (1-64). | |
| Texture Anisotropic Filtering<br>`eldr:anisotropy` | int 16 | Texture sharpness at grazing angles (1 = off, 16 = most). | Small |
| Texture Compression<br>`eldr:textureCompression` | off | Compresses 8-bit material textures on the GPU (cached on disk) to save memory, with a small loss. 16-bit, float and light textures stay exact. Off: exact, as Karma. | 4x less texture memory (gray: 2x); crowd_9k 172 to 112 MB; 52 dB PSNR vs exact |
| Textured Transmission and Subsurface Inputs<br>`eldr:texturedTransmission` | on | Transmission, colour, depth, thin walled, subsurface weight, colour, radius and scale may be textures or graphs. Off reads constants. | |
| Translucent Objects<br>`eldr:translucency` | on | Draws materials with opacity below 1. Off: not drawn. | |
| Order-Independent Transparency<br>`eldr:oit` | on | Blends overlapping transparent surfaces without sorting them. Off: sorts whole meshes back to front, which can misorder parts of one mesh. | 18 bytes per pixel, one pass |
| OIT Depth Weight<br>`eldr:oitDepthWeight` | float 4 | Distance weight exponent (0 = plain average). Raise if far layers show through near ones. | |
| Refraction<br>`eldr:refraction` | on | [prepass] Glass and water bend and tint what is behind them. Only what is visible on screen refracts. | Glass scene 0.2 ms |
| Refraction Blur Samples<br>`eldr:refractionSamples` | int 8 | Taps for rough glass (1-16), screen-space refraction only. Ray-traced refraction (`eldr:rtRefraction`) blurs over film samples instead. | |
| Refraction Back Faces<br>`eldr:refractionBackFaces` | on | Draws back faces (water from below, Snell's window, total internal reflection). Off discards them. | |
| Refraction through Layers<br>`eldr:multiRefraction` | on | Glass behind glass refracts correctly. | 0.1 ms per layer |
| Refraction Layers<br>`eldr:refractionLayers` | int 4 | Most glass layers handled per frame (1-16); further layers share the last. Volumes see up to 4. | Volumes under glass: 8 bytes per pixel per layer |
| Holdouts<br>`eldr:holdouts` | on | Matte objects render black with zero alpha, but keep their depth, ids and shadows. | None |
| Translucent Holdouts Cut Alpha<br>`eldr:holdoutAlpha` | on | A see-through holdout cuts colour and alpha by its opacity. | Small pass when present |
| Opacity Mode Presence<br>`eldr:opacityMode` | on | UsdPreviewSurface `opacityMode` "presence" fades the whole surface, highlights included, with no refraction; "transparent" keeps highlights. Off: both act as transparent. | Free |

### Geometry

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Levels of Detail<br>`eldr:lod` | on | Meshes of 1000+ triangles get up to 3 simpler versions; each instance draws the simplest one that looks the same (under about 1 pixel of error). Translucent and displaced meshes keep full detail. Turn off for pixel-exact comparisons. | Crowd 68 to 28-34 ms; Kitchen at Very High 30 to 10.5 ms |
| Level of Detail Bias<br>`eldr:lodBias` | float 0 | +1 allows twice the error, -1 half. Use negative values for finals. | -1: crowd 13.6M instead of 9.6M triangles |
| Level of Detail Crossfade<br>`eldr:lodFade` | on | Blends a switch between detail levels over 8 frames instead of popping. Without TAA the blend shows as a dither pattern. | Crowd +0.05 ms |
| Subdivision Surfaces<br>`eldr:subdivision` | on | Smooths subdivision meshes (catmullClark, loop, bilinear) to Hydra's refine level (usdview Complexity, Houdini's Level of Detail) or the mesh's `primvars:eldr:refineLevel`. Off draws the control cage. | 4x memory per level; Kitchen at Very High: 35M triangles, 5.5 s load |
| Subdivision Max Level<br>`eldr:maxRefineLevel` | int 3 | Clamps the refine level (0-8). | |
| Dicing by Screen Size<br>`eldr:screenDicing` | on | Picks each subdivision mesh's level from its size on screen, like Karma's dicing, set by `primvars:eldr:object:dicingquality` (else Karma's, 0.5). Off: the display level. | Kitchen in husk (960x540, level 3), off to on: 34.8M to 8.4M triangles, peak RAM 4.3 to 2.2 GB, load 11.9 to 3.2 s |
| Displacement<br>`eldr:displacement` | on | Moves surface points by the material's displacement (UsdPreviewSurface, MaterialX). Needs enough vertices: use subdivision. | Only displaced materials pay |
| Displacement Scale<br>`eldr:displacementScale` | float 1 | Multiplies every displacement. | Rebuilds displaced ray-tracing data |
| Displacement Graphs<br>`eldr:displacementGraphs` | on | Allows displacement driven by shading networks (math, noise, several textures, primvars). Off: such materials don't displace. | Graph-displaced surfaces shade with flat facet normals; ray tracing builds extra data per mesh or instance |
| Curves as Camera-Facing Ribbons<br>`eldr:curveRibbons` | off | Draws curves without normals as flat ribbons facing the camera instead of round tubes. `primvars:eldr:ribbons` (bool) overrides it per curve. | 2 triangles per step instead of 12; 100k blades 14.9 to 9.6 ms. Ray tracing sees lines. |
| Points as Camera-Facing Discs<br>`eldr:pointSprites` | off | Draws points as shaded discs instead of small spheres. `primvars:eldr:sprites` (bool) overrides it per prim. | 8 triangles per point instead of 20. Ray tracing sees points. |

### Volumes and Fog

Fog is height fog set only here (USD has no fog schema). Volumes are OpenVDB `UsdVolVolume` prims and need a USD build with OpenVDB.

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Fog Density<br>`eldr:fogDensity` | float 0.02 | Extinction per scene unit at base height. | |
| Fog Height<br>`eldr:fogHeight` | float 0 | Base height (Y, or Z with `eldr:fogZUp`). | |
| Fog Height Falloff<br>`eldr:fogHeightFalloff` | float 0 | Density x exp(-falloff x (height - base)); 0 = uniform. | |
| Fog Anisotropy<br>`eldr:fogAnisotropy` | float 0.6 | Henyey-Greenstein g (-1..1); towards 1: shafts toward lights. | |
| Fog Ambient from GI Probes<br>`eldr:fogGiAmbient` | on | With GI on, fog takes its sky light from the GI probes, so closed rooms have no outdoor haze. | 0.2 to 0.47 ms with GI |
| Fog Color<br>`eldr:fogColor` | string "1 1 1" | Scattering albedo "r g b". | |
| Fog Max Distance<br>`eldr:fogMaxDistance` | float 0 | Froxel depth; 0 = scene far end. Shorten for detail near the camera. | |
| Fog Grid Size<br>`eldr:fogGridSize` | int 160 | Fog grid cells across the image (16-512), with 64 depth slices. | 64: 0.06 ms; 256: 0.41 ms |
| Fog Height along Z<br>`eldr:fogZUp` | off | Height along Z for Z-up stages. | |
| Fog Film Jitter<br>`eldr:fogFilmJitter` | on | [film] Samples the fog at a random point in each cell per sample, so the converged fog is smooth. | 64 samples: RMSE 0.0014 to 0.00022 |
| Volume Step Size<br>`eldr:volumeStepSize` | float 1 | March step in voxels; larger is faster, coarser. | |
| Volume Max Steps<br>`eldr:volumeMaxSteps` | int 256 | Steps per pixel and volume box. | |
| Volume Shadow Steps<br>`eldr:volumeShadowSteps` | int 8 | Steps towards the light for each volume sample's shadow. | volume.usda 21 ms at 8, about 17 at 4, about 11 at 1 |
| Volume Local Shadow Stride<br>`eldr:volumeLocalShadowStride` | int 2 | Looks up local lights' shadows inside volumes every this many steps (1-16) and reuses them in between. 1: every step. | volume + 3 sphere lights: 257.9 ms at 1, 237.3 at 2 (63 dB PSNR) |
| Volume Density Multiplier<br>`eldr:volumeDensity` | float 1 | Scales density (extinction). | |
| Volume Emission Multiplier<br>`eldr:volumeEmission` | float 1 | Scales emission. | |
| Volume Phase Anisotropy<br>`eldr:volumeAnisotropy` | float 0 | Phase g for volumes without a MaterialX volume material (0: isotropic, as Karma). | |
| Volume Resolution<br>`eldr:volumeResolution` | int 1 | 1 = full resolution, 2 = half (upsampled). | volume.usda 22.0 ms at 1, 7.3 at 2 |
| Volumes under Transparency<br>`eldr:volumeUnderTransparency` | on | Smoke behind glass is dimmed by each pane in front of it and refracted (up to `eldr:refractionLayers` layers). | 5.8 vs 4.2 ms off |
| Volume Shadows<br>`eldr:volumeShadows` | on | Volumes cast shadows from the sun onto surfaces and fog. | volume.usda 1.0 vs 0.07 ms |
| Local Volume Shadows<br>`eldr:localVolumeShadows` | on | Volumes cast shadows from local lights too. Needs `eldr:volumeShadows` and `eldr:localShadows`. | Spot 0.17 ms, two sphere lights 0.8 ms |
| Volume Mutual Shadows<br>`eldr:volumeMutualShadows` | on | Volumes shadow each other, from the sun, local lights and the dome. | Three volumes 35.2 to 39.2 ms; a lone volume is free |
| Volume Shadow Grid<br>`eldr:volumeShadowGrid` | int 64 | Precomputes each volume's shadowing on a grid of this many points per axis (8-128) instead of tracing it per ray. 0: march every ray (slow). | volume + 3 lights, 1080p: 234 ms at 0, 77 at 64; up to 64 MB |
| Volume Default Fields<br>`eldr:volumeDefaultFields` | off | Volumes without a volume material also read Cd (tints scattering), temperature (blackbody glow) and emission. Off: density only, as Karma. | |
| Volume Velocity Blur<br>`eldr:volumeVelocityBlur` | on | Volumes with Velocity Blur (`primvars:eldr:` or `karma:object:vblur` above 0) blur along their `vel` field, as Karma. Off: sharp. | Slower stepping while blurring |

### Camera and Film

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Temporal Anti-Aliasing<br>`eldr:taa` | on | Jitters the camera and resolves history. The film needs it too. Off: one aliased sample. | |
| Film Pixel Filter Radius<br>`eldr:filmFilterRadius` | float 1.5 | [film] Gaussian radius in pixels (0-2). | |
| Object Motion Vectors<br>`eldr:objectMotion` | on | Tracks moving objects for TAA and motion blur. Off: camera motion only. | About 4 ms on crowd |
| Blend Shape Motion Vectors<br>`eldr:blendShapeMotion` | on | Blend shape weight changes count in skinned motion. | |
| Sampled Motion<br>`eldr:sampledMotion` | on | Takes motion from the scene's animation across the shutter, so a single frame blurs on its own. Off: motion since the last frame. Rigid motion only. | Two extra transform reads per moving prim |
| Deformation Blur<br>`eldr:deformationBlur` | on | Deforming meshes, curves and points (caches, simulations, `velocities` with Velocity Blur) blur too. Skinned meshes have their own motion. | Moving meshes: 16 bytes per vertex |
| Residual Motion Blur<br>`eldr:motionBlurResidual` | on | [film] Adds a short blur between motion blur time samples, so blur looks smooth, not like separate copies. | Only while something moves over 1 pixel per sample |
| Lens Jitter per Sample<br>`eldr:dofJitter` | on | [film] Each sample looks through a different point of the lens, for accurate depth of field and bokeh. | None |
| Residual DOF Blur<br>`eldr:dofResidualBlur` | on | [film] Adds a little post blur to lens jitter, which hides gaps at low sample counts and fades as samples grow. | About 1.9 ms per sample at 1080p, with DOF |
| Lens Distortion<br>`eldr:lensDistortion` | on | Applies the camera's `lensDistortion:*` and OpenCV lens terms to the finished image, AOVs included. | 0.32 ms at 1080p; none without distortion |
| Post Processing<br>`eldr:postProcess` | on | Exposure, bloom, tonemap; TAA, film, SSR, post DOF, motion blur and upscaling depend on it. Off: raw linear HDR, one aliased sample. For linear EXRs keep on with `eldr:tonemapper` `none`, `eldr:autoExposure` off, `eldr:bloomIntensity` 0. | |

## AOVs and Products

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Extra AOVs<br>`eldr:extraAovs` | on | Renders AOVs beyond colour, depth and ids when they are requested. | None unless requested; a requested AOV disables `eldr:renderScale` |
| Unread AOVs Once Converged<br>`eldr:lazyExtraAovs` | on | AOVs that are requested but not displayed (Houdini requests normal and st) render once when the image converges, not every frame. | macbeth 1866x737: 4.9 ms less per still frame |
| Cryptomatte Soft Edges and Ranks<br>`eldr:cryptomatteSoft` | on | [film] Cryptomatte AOVs keep the 6 most-covering objects per pixel, with soft edges, in Karma's layout. Off or while moving: one object per pixel. | 14 floats per pixel, only with a Cryptomatte AOV |
| Per-AOV Sample Filters<br>`eldr:aovFilters` | on | [film] Each render var's `driver:parameters:aov:eldr:filter` (else Karma's `...karma:filter`) picks how samples combine: beauty, nearest, gauss, box, min, max, closest, farthest. Off: colour like the beauty, data nearest. | None |
| Dome as Direct in Light Splits<br>`eldr:splitDomeDirect` | on | In light AOVs, counts unblocked dome light as direct light, as Karma. Off: all dome light is indirect. The beauty doesn't change. | Only with light AOVs |
| Write Render Products<br>`eldr:renderProducts` | on | Writes the render settings' EXR files when the frame has converged (husk, `usdrecord --renderSettingsPrimPath`). Off writes nothing. | One readback per converged frame |
| Render Products at their Resolution<br>`eldr:productResolution` | on | Renders at the first image product's `resolution`, `pixelAspectRatio` and `dataWindowNDC` (no `resolution`: 2048x1080, as husk). Off: the window size. | One CPU resample per frame while sizes differ |

## Performance and Debug

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Depth Prepass<br>`eldr:depthPrepass` | on | Draws depth first so each pixel shades once. AO, SSR, SSGI, refraction, subsurface and ray tracing need it. Turn off only for comparisons. | Off saves 14 ms on crowd but drops those effects |
| Frustum Culling<br>`eldr:frustumCulling` | on | Culls instances outside the view. Off for A/B tests. | |
| Small Object Culling<br>`eldr:cullPixels` | float 0.5 | Skips instances under this screen radius in pixels; 0 = off. Use 0 for finals with tiny props. | |
| Shadow Small Object Culling<br>`eldr:shadowCullPixels` | float 0.5 | Sun shadows skip instances under this radius in shadow texels of their cascade (far cascades have big texels, so thousands of distant agents drop out). 0 = off. | |
| Offline Samples per Execute<br>`eldr:offlineBatchSeconds` | float 1 | husk and usdrecord render film samples back to back for up to this many seconds before reporting progress. 0: one sample at a time. | husk crowd 9k 480x270: 13.3 s at 0, 6.9 s at 1 |
| Share Frames with OpenGL<br>`eldr:shareFrame` | on | usdview and usdrecord show the image straight from the GPU, without a copy through the CPU. Falls back to a copy when unsupported. | 1080p 17.4 to 3.2 ms; 4K 67 to 9.6 ms |
| Overlap Readback while Moving<br>`eldr:overlapReadback` | on | In Houdini, while the camera moves, shows each frame one frame late so the GPU and Houdini work at the same time. A still camera's frame is never late. | step4 1866x737: 20.3 to 14.0 ms a frame |
| Read Back in Place<br>`eldr:inPlaceReadback` | on | In Houdini, hands over the image without an extra copy. | macbeth 1866x737, 4 AOVs: 14.6 to 8.2 ms a frame |
| Keep Ids while Moving<br>`eldr:staleMovingIds` | on | In Houdini, skips the object ids used for picking while the camera moves; a still camera reads them. | macbeth in Houdini 22: 90 to 104 fps |
| Pipeline Cache on Disk<br>`eldr:pipelineCache` | on | Saves compiled shaders to `%LOCALAPPDATA%\Eldr\pipelines-*.bin` (`HDVK_PIPELINE_CACHE_DIR` moves it), so later sessions start fast. | Crowd first frame 33.5 s cold, 2.0 s cached; file about 16 MB |
| Prewarm Pipelines<br>`eldr:pipelinePrewarm` | on in Hydra, off for embedders | Compiles the shaders for likely toggles (GI, SSGI, fog) in the background, so turning them on doesn't freeze the viewport. Off in husk and usdrecord. | Half the CPU cores in the background |
| Compile Pipelines on Demand<br>`eldr:lazyPipelines` | on | Compiles only the shaders the scene uses, when first needed. Off: compiles every variant up front. | macbeth, cold cache: first frame 34.3 s off, 6.1 s on |
