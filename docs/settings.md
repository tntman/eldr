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
| [film] | Acts only while the film accumulates: camera still, and in final renders (tooltip: "still camera and final renders") |
| [interactive] | Acts only while the camera or scene moves (tooltip: "while moving only") |
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
| Film Samples<br>`eldr:filmSamples` | int 64 | Samples per pixel the image collects while the camera is still: more samples, less noise. The viewport and final renders use the same count. Use 256 or more for noise-free finals. Like Karma's Pixel Samples. [film] | Linear |
| Film Restarts at Once<br>`eldr:filmRestart` | off | On: any change (camera, object, setting) restarts the image from its first sample, so the viewport shows exactly the frames a final render makes. Off: smooth TAA while things move, and the image starts refining after a few still frames. | Interactive: no TAA upscaling, a fresh film per change |
| Denoise<br>`eldr:denoise` | off | Removes the remaining noise from the finished image with Intel Open Image Denoise (CPU, about 1 s per 1080p frame). Can smear fine detail; AOV eldr:beautyRaw keeps the noisy image. Karma's `karma:hydra:denoise` also turns it on. [film] | 1080p: about 1.3 s once per frame (CPU); guide pass +10 ms per sample |
| Diffuse Quality<br>`eldr:diffuseQuality` | float -1 (unset: Karma's, else 1) | Multiplies the rays for bounced diffuse light (final gather, GI probes, AO, SSGI): raise it for cleaner indirect light, lower it for speed. As Karma's Diffuse Quality. Unset (-1): Karma's value if the scene has one, else 1. | emissive.usda 960x540: 15 / 20 / 35 ms per sample at 0.5 / 1 / 2; 2 at 32 samples is as clean as 64 |
| Reflection Quality<br>`eldr:reflectQuality` | float -1 (unset: Karma's, else 1) | Multiplies the ray-traced reflection rays: raise it for cleaner glossy reflections, lower it for speed. As Karma's Reflection Quality. Unset (-1): Karma's value if the scene has one, else 1. | Linear in the reflections |
| Refraction Quality<br>`eldr:refractQuality` | float -1 (unset: Karma's, else 1) | Multiplies the rays through rough glass (ray-traced) and the blur taps of screen-space refraction (up to 16): raise it for cleaner frosted glass. As Karma's Refraction Quality. Unset (-1): Karma's value if the scene has one, else 1. | glass.usda 960x540, traced: 19.9 / 22.5 / 33.1 ms per sample at 1 / 2 / 4; 2 at 16 samples is as clean as 1 at 32. Screen space: 4.9 / 5.2 / 5.4 ms |
| Light Quality<br>`eldr:lightQuality` | float -1 (unset: Karma's, else 1) | Multiplies the ray-traced shadow rays per light: raise it for cleaner soft shadows. Shadow maps don't change. As Karma's Light Sampling Quality. Unset (-1): Karma's value if the scene has one, else 1. | Linear in the traced shadows |
| Subsurface Quality<br>`eldr:sssQuality` | float -1 (unset: Karma's, else 1) | Multiplies the subsurface blur samples (Subsurface Samples): raise it if skin, wax or marble looks blotchy. As Karma's SSS Quality. Unset (-1): Karma's value if the scene has one, else 1. | Linear in the subsurface blur |
| Volume Quality<br>`eldr:volumeQuality` | float -1 (unset: Karma's, else 1) | Multiplies the steps towards the light inside volumes (Volume Shadow Steps): raise it for more accurate self-shadowing in smoke and clouds. As Karma's Volume Quality. Unset (-1): Karma's value if the scene has one, else 1. | Linear in the volume lighting |
| Tile Size<br>`eldr:tileSize` | int 0 (off) | Splits husk and usdrecord renders into square tiles of this many pixels (rounded up to 64), rendered one after another, so GPU memory depends on the tile, not the image. Use it for very large images. 0: the whole image at once. Viewports ignore it. | husk, stadium with 9k crowd, 6000x3000 at 2048: peak GPU 4.3 to 2.8 GB, 61 to 71 s; 12000x12000 at 2048: 6.2 GB, 155 s |

## Ray Tracing

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Ray-Traced Shadows<br>`eldr:rtShadows` | off | Ray-traced shadows instead of shadow maps, for the sun and the most important local lights (Ray-Traced Shadow Lights). Exact contact shadows, softness from each light's size; noise averages out as the image refines. [RT] [prepass] | Kitchen about 10 ms |
| Ray-Traced Shadows when Cheaper<br>`eldr:rtShadowsAuto` | off | While Ray-Traced Shadows is off, traces shadows anyway where that is likely faster than shadow maps: dense scenes (2 or more triangles per pixel and light) whose skinned characters all fit in ray tracing. [RT] | 960x540, 64 samples, traced / maps: City 2.2 / 4.5 s; crowd 0.8 / 1.4 s; Kitchen 0.93 / 0.64 s |
| Ray-Traced Reflections<br>`eldr:rtReflections` | off | Reflections from ray tracing on surfaces up to roughness 0.7, instead of the dome and screen-space reflections, so objects off screen or hidden from the camera show in them. Use it for finals with glossy surfaces. [RT] [prepass] | Kitchen about 15 ms |
| Ray-Traced Reflection Bounces<br>`eldr:rtReflectionBounces` | int -1 (unset: Karma's, else 4) | Rays one reflection path may take (1-16): glossy objects in a traced reflection reflect again (a ball on a mirror floor shows the floor); past the last: black, as Karma. Unset: Karma's Reflection Limit (`karma:global:reflectlimit`), else 4. [RT] | refl_test 960x540, 64 samples (husk): 4.7 s at 1, 4.6 s at 4; more where glossy objects face each other |
| Ray-Traced Refraction<br>`eldr:rtRefraction` | off | Traces rays through glass and water, so back faces, glass behind glass, total internal reflection, absorption and off-screen objects all show. Rough glass blurs as the image refines. Off: screen-space refraction. [RT] [prepass] | glass.usda 8.1-9.7 ms; crowd unchanged |
| Ray-Traced Refraction Interfaces<br>`eldr:rtRefractionBounces` | int -1 (unset: Karma's, else 4) | Glass surfaces that bend a ray (1-16): 2 for one ball, 4 for a ball behind a pane. Past them it goes straight through (Karma's Thinwall at Refract Limit). Unset: Karma's Refraction Limit (`karma:global:refractlimit`), else 4. [RT] | |
| Ray-Traced Hit Shadows<br>`eldr:rtHitShadows` | off | Objects seen in ray-traced reflections, refraction and final gather get shadows from all distant lights and up to 16 local lights. Off: only distant lights shadow them, so a floor in a lamp's shadow can look lit in a reflection. [RT] | Up to one ray per light per hit |
| Final Gather<br>`eldr:finalGather` | off | Traces a ray per pixel and sample for bounced diffuse light, instead of the GI probes, AO and SSGI there. Gives accurate contact shadows, dark corners and colour bleeding. For finals. Needs Global Illumination on. [RT] [prepass] | 1080p, 64 samples: emissive.usda +0.34 s, Cornell +0.15 s per frame; interactive +5.5 ms per frame |
| Caustics<br>`eldr:caustics` | off | Light focused through glass or off mirrors onto diffuse surfaces (photon mapping), from sun, sphere, rect and disk lights. Only objects with Karma's Enable Caustics primvar cast them. Sharpens as the image refines; AOV `caustics`. [RT] | caustics.usda 960x540: 5.9 ms per sample off, 13.3 ms on (65536 photons) |

## Global Illumination

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Global Illumination<br>`eldr:gi` | off | Bounced diffuse light from a grid of ray-traced probes, so light spills from lit areas into shadowed ones. Use it for interiors and scenes lit by emissive surfaces; add Final Gather for finals. [RT] | Kitchen 2-3 ms (1728 probes x 128 rays); crowd 0.2 ms; plus ray-tracing memory |
| Diffuse Limit<br>`eldr:diffuseLimit` | int -1 (unset: Karma's, else 1) | How many diffuse bounces GI's light takes, as Karma's Diffuse Limit: higher brightens interiors. 0: the dome's direct light only; above 16: no limit. Unset (-1): Karma's `karma:object:diffuselimit`, else `karma:global:diffuselimit`, else 1. [RT] | None. Cornell box, Eldr final over Karma: x1.00 at 1 and 2, x1.02 at 8 |
| Screen-Space GI<br>`eldr:ssgi` | off | Bounced light and colour bleeding taken from what is visible on screen. Works without ray tracing, but misses light from off-screen and hidden surfaces. Replaces Ambient Occlusion when on; Diffuse Limit 0 keeps its occlusion only. [prepass] | Kitchen 1080p, half res: 0.83 ms |
| Ambient Occlusion<br>`eldr:ambientOcclusion` | on | Darkens the dome's and GI probes' light in corners and where objects touch. SSGI replaces it when on; Final Gather, where it runs. Off: a flatter look. [prepass] | 0.34 ms |
| Dome Lighting<br>`eldr:imageBasedLighting` | on | Lights the scene with the dome light's image, both diffuse light and reflections. Off: only the other lights light the scene. The Reflection Probe needs it on. | Small |
| Dome Sun Extraction<br>`eldr:domeSun` | on | Finds the sun in an HDRI dome image and turns it into a distant light that casts sharp shadows. Off: the dome still lights the scene, but its sun casts no shadows. | CPU work when the dome changes |
| Dome Sun Threshold<br>`eldr:domeSunThreshold` | float 10 | How bright a spot in the dome image must be to count as the sun. Lower it if Dome Sun Extraction misses the sun. | |

## Camera

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Motion Blur<br>`eldr:motionBlur` | on | Blurs moving objects and camera motion over the camera's shutter (`shutter:open`, `shutter:close`), as Karma. While you move in the viewport, a fast post-process blur stands in. | Post blur 0.2 ms |
| Motion Blur Time Samples<br>`eldr:motionBlurTimeSamples` | on | Renders each sample at its own time within the shutter, so shadows, reflections and refraction blur correctly too. Off: a post-process blur of the finished image, faster but less exact. [film] | Crowd 1.7-2.1 ms per sample |
| Depth of Field<br>`eldr:depthOfField` | on | Lens blur from the camera's `fStop`, `focalLength` and `focusDistance`, as Karma. Perspective cameras only; usdview's free camera has none. | Post DOF about 1.9 ms at 1080p |
| Camera Exposure<br>`eldr:cameraExposure` | on | Applies the USD camera's exposure (`exposure`, `exposure:iso`, `:time`, `:fStop`, `:responsivity`) to the beauty and light AOVs, as Karma; data AOVs stay unchanged. Exposure Compensation adds on top; colour limits apply after it. | |

## Image

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Exposure Compensation<br>`eldr:exposureCompensation` | float 0 | Brightens (+) or darkens (-) the image in stops (EV): +1 doubles the brightness. Adds to the camera's exposure and applies also with Linear Beauty on. With Auto Exposure off it is the only extra exposure. | |
| Linear Beauty<br>`eldr:productLinear` | on | Gives Houdini's viewport and husk the plain linear scene colour, as Karma: no tonemapper, bloom or auto exposure unless the render settings prim sets them. Keep on for EXRs and compositing. Off: Eldr's tonemapped display image. | One extra pass |
| Auto Exposure<br>`eldr:autoExposure` | off | Adjusts brightness over time from the image's average, like a camera's auto exposure. Off: fixed exposure from Exposure Compensation, as Karma. | 0.11 ms |
| Tonemapper<br>`eldr:tonemapper` | string aces | Maps bright HDR colour to the display: `aces` (filmic), `agx` (softer highlights), `agxPunchy` (more contrast and colour) or `none` (linear, unclamped). Ignored while Linear Beauty is on (Houdini viewport, husk products). | |
| Bloom Intensity<br>`eldr:bloomIntensity` | float 0.04 | Glow around bright areas: the share (0-1) of a blurred copy mixed into the image. 0: none. Ignored while Linear Beauty is on (Houdini viewport, husk products). | |

## Volumes and Fog

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Volumetric Fog<br>`eldr:fog` | off | Height fog lit by all lights, with light shafts. Set its density, height and colour under Advanced: Volumes and Fog. | 0.19 ms on crowd; shading +0.5 ms |
| Volumes<br>`eldr:volumes` | on | Renders OpenVDB volumes such as smoke, clouds and fire. Off: volumes are skipped. Needs a USD build with OpenVDB. | See Fog and volumes |
| Volume Limit<br>`eldr:object:volumelimit` | int -1 (unset: Karma's, else 0) | How many times light may scatter inside a volume, as Karma's Volume Limit: higher brightens thick clouds and smoke. 0: once (Karma's default). Unset (-1): Karma's value, else 0. A volume's own Render Geometry Settings value wins. | 0: none. 16: volume pass 12.1 to 13.0 ms (29_26, 960x704). Within 9% of Karma at limits 1, 4, 16 (backlit sun-only up to 23% too bright) |

## Viewport

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Film when Still<br>`eldr:filmAccumulation` | on | After 3 still frames, switches from TAA to the film: an exact average of samples that refines into the final image. Off: TAA only, also when still, which never fully refines. | 0.23 ms |
| TAA Samples<br>`eldr:taaSamples` | int 16 | Length of TAA's camera jitter pattern. Without the film, a still camera averages this many frames, and AOVs average at most this many. Higher: smoother edges, slower to settle. | |
| Still Delay<br>`eldr:stillDelay` | float 0.3 | Seconds (0-30) a stopped camera still counts as moving, so short pauses while you tumble don't flip to full quality and back. 0: full quality from the first still frame. Offline renders ignore it. [interactive] | crowd 9k / Kitchen / emissive RT: see docs/guide/viewport.md |
| Render Scale<br>`eldr:renderScale` | float 1 | Renders smaller (0.25-1) whenever TAA runs instead of the film (while moving, or always with Film when Still off), and TAA upscales to full size: a faster viewport. The film, lens distortion and requested AOVs keep full size. | Crowd moving: 27.0 ms at 1.0, 23.0 at 0.75, 16.4 at 0.5 |
| Render Scale while Moving<br>`eldr:movingRenderScale` | float 0.5 | While the camera or scene moves, renders at most at this scale (0.25-1) for a faster viewport; with a Frame Budget set, the scale it starts from. 1: full size. [interactive] | Moving, 1920x1080: Kitchen 21.6 to 8.6 ms, crowd_9k 37.4 to 26.7, emissive RT 64.2 to 12.5 (with the two below) |
| Interactive Noise Loop<br>`eldr:interactiveSamples` | int 8 | While TAA runs, the random noise pattern repeats every this many frames (1-256), so TAA settles on a steady image instead of crawling noise. Lower: calmer; higher: more detail over time. [interactive] | |
| Ray-Traced Shadow Rays while Moving<br>`eldr:movingShadowRays` | int 1 | Ray-traced shadow rays per light while moving; TAA smooths the extra noise. 0: as many as when still. Raise it if shadows crawl while you move. [interactive] [RT] | emissive 1080p moving: 11.7 to 5.1 ms (4 to 1 rays) |
| Pause Settled GI Probes while the Camera Moves<br>`eldr:movingGiPause` | on | While only the camera moves, settled GI probes stop updating, since their light doesn't depend on the camera. Saves time while you tumble. [interactive] [RT] | emissive giProbes 4.0 ms to 0 while moving |
| Ray-Traced Temporal Reuse<br>`eldr:rtTemporalReuse` | off | While moving, blends ray-traced shadows, rough reflections and final gather with earlier frames at the same surface point: calmer soft shadows, but thin contact shadows soften. The film and offline renders never use it. [interactive] [RT] | 1866x737 moving: 0.2-0.4 ms; RMS against the converged frame about the same as TAA alone (emissive, Kitchen) |
| Ray-Traced History Frames<br>`eldr:rtTemporalFrames` | int 16 | The most frames Ray-Traced Temporal Reuse blends (1-64). Higher: smoother, but slower to follow moving shadows and reflections. [interactive] [RT] | |
| Ramp to Full Quality<br>`eldr:rampFrames` | int 6 | Frames over which quality steps back up after moving (render scale, shadow rays, GI probes) before the film starts, so the view doesn't jump. 0 or 1: at once. Offline renders ignore it. [interactive] | Houdini 1866x737, emissive RT: worst frame after the stop 41.7 to 31.7 ms; full quality 0.33 to 0.47 s after the stop. Each step resizes the targets (about 3 ms CPU) |
| Frame Budget while Moving<br>`eldr:movingFrameBudget` | float 33 | Target GPU milliseconds per frame while moving: the render scale adapts (0.25-1), so light scenes stay full size and heavy ones shrink. 0: use Render Scale while Moving instead. Offline renders ignore it. [interactive] | Moving, 1866x737: crowd_9k RT 80 to 36 ms (quarter size), Kitchen 9.4 to 17.6 ms (full size), emissive RT 11.5 to 27.6 ms |
| Film Warm Start<br>`eldr:filmWarmStart` | float 4 | The film starts from the last TAA image, counted as this many samples, so its first frames are already clean. 0: a clean start. Film Restarts at Once always starts clean. [interactive] | None |

## Advanced

Quality controls (steps, radii, resolutions, thresholds) and the remaining on/off switches.

### Sampling

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Sample Dimensions<br>`eldr:sampleDimensions` | on | Gives each random effect (lens, shadows, rays) its own well-spread sample pattern, so noise shows no patterns and clears faster. Off only to compare. | None |
| Adaptive Sampling Noise Threshold<br>`eldr:adaptiveThreshold` | float 0 (off) | Lets pixels stop sampling early once their noise falls under this value, so clean areas finish sooner. Try 0.002 for large flat areas. 0: off, every pixel takes all Film Samples. Not used with Motion Blur Time Samples. [film] | Off is bit-identical |
| Adaptive Sampling Converged Pixel Fraction<br>`eldr:adaptivePixelFraction` | float 0.999 | With adaptive sampling, the image counts as finished once this fraction (0-1) of its pixels has stopped. 1: waits for every pixel. [film] | Cornell at 0.01: 27 samples at 0.9, 58 at 0.95 |
| Adaptive Sampling Min Samples<br>`eldr:adaptiveMinSamples` | int 16 | With adaptive sampling, the samples every pixel takes before it may stop. Raise it if small bright details stop too early and stay noisy. [film] | |
| Adaptive Sampling Skips Stopped Tiles<br>`eldr:adaptiveTiles` | on | With adaptive sampling, blocks of pixels that have all stopped skip their work, so later samples are cheaper. The image is the same. Not used where refraction, subsurface or post depth of field is in view. [film] | Kitchen 1080p, threshold 0.01: 17.4 to 12.8 s |
| Effect Denoise Fade<br>`eldr:effectDenoiseSamples` | int 64 | Blurs the noise of final gather and ray-traced shadows and reflections on the first samples, fading out over this many: early frames look clean, the finished image stays sharp. 0: blur always (slightly soft). Off with Denoise. [film] | At 4/16/64 samples, RMSE raw vs blurred: emissive.usda 0.027/0.023/0.012 vs 0.009/0.008/0.008 |
| Denoise Guide Samples<br>`eldr:denoiseGuideSamples` | int 32 | The film samples whose albedo and normal guide Denoise; later samples skip drawing them. 0: every sample. Not used when the albedo or normal AOV is rendered. [film] | 256 samples, gallery Curves 960 x 540: 1.30 to 0.80 s; mean pixel difference 0.0002 |
| Direct Light Clamp<br>`eldr:directClamp` | float 0 (off) | Caps how bright direct light can make a pixel, keeping its hue, to remove fireflies. Costs some energy, so highlights get dimmer. 0: off. | |
| Indirect Light Clamp<br>`eldr:indirectClamp` | float -1 (unset: 20) | Caps bounced light (SSGI, SSR, ray-traced hits, GI rays), the emission it sees and emissive-mesh lights, to remove fireflies; the dome is never capped. As Karma's Indirect Color Limit. 0: off. Unset (-1): from Color Limit, else 20. | |
| Color Limit<br>`eldr:colorLimit` | float -1 (unset: Karma's, else 20) | Caps every camera sample (emitters, highlights, reflections), keeping its hue (40 shows as 20), as Karma's Color Limit; also sets the bounced-light cap (see Indirect Clamp Shared). Unset (-1): Karma's, else 20. 0: no cap. Karma's 0 is ignored. | |
| Indirect Clamp Shared with Color Limit<br>`eldr:colorLimitShared` | int -1 (unset) | Whether Indirect Light Clamp follows Color Limit: 1 yes; 0 no, it uses Karma's `karma:global:colorlimitindirect` (else Color Limit). Unset (-1): Karma's `colorlimitindlinked` decides (default yes). Indirect Light Clamp, when set, wins. | |

### Ray Tracing

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Ray-Traced Light Sampling<br>`eldr:rtLightSampling` | on | Ray-traced rays sample one light at a time, picked by brightness, and skip lights that can't reach a pixel. The same finished image, much faster with many lights. Off only to compare. [RT] | 1080p, ms per sample: emissive 147 to 78, Kitchen 195 to 158, crowd 542 to 478; noise unchanged |
| Ray-Traced Skinned Instances<br>`eldr:rtSkinnedInstances` | int 16 | How many of the largest skinned characters (0-256) appear in ray-traced shadows, reflections and GI. Raise it for crowds whose far characters should block rays; costs memory and time. [RT] | Kitchen 0.45 ms (16), 0.69 ms (90); crowd 0.68 ms (16), 2.7 ms (256) |
| Ray Tracing Sees Displacement<br>`eldr:rtDisplacement` | on | Ray-traced shadows, reflections and GI see displaced surfaces. Off: rays see the undisplaced shape, which is faster to update when displacement animates. [RT] | Rebuilds when the displacement changes |
| Ray Tracing Compacts Static BLASes<br>`eldr:rtCompaction` | on | Shrinks the ray-tracing data of static meshes after it is built, about halving its memory. The image is the same. Off only to compare. [RT] | Applies to data built after the change |
| Final Gather Distance<br>`eldr:finalGatherDistance` | float 0 | How far final gather rays travel, in scene units; past it the GI probes supply the light. Faster, but less exact in large spaces. 0: rays travel until they hit something. [RT] | emissive.usda 960x540, 256 samples: 5.2 s at 0, 3.9 s at 0.5 |
| Final Gather Rays per Pixel<br>`eldr:finalGatherRays` | int 1 | Final gather rays per pixel and sample (1-16): more give cleaner bounced light per sample. With Final Gather Denoise on, more than 1 gains little. [RT] | 1080p, 64 samples: emissive.usda 2.7 s at 1, 5.3 s at 2, 10.4 s at 4 |
| Final Gather Denoise<br>`eldr:finalGatherDenoise` | float 1 | Strength (0-1) of an edge-aware blur on the gathered bounced light during the first Effect Denoise Fade samples; textures stay sharp. 0: raw rays. [RT] | About 1 ms per sample at 1080p while active |
| Final Gather Roulette<br>`eldr:finalGatherRoulette` | off | From the second sample, each 8x8 pixel block gathers on every other sample, weighted to match: faster samples, about 1.5x the noise at 64 samples. The finished image is the same. [RT] | 1080p, ms per sample: emissive 78 to 64, Kitchen 158 to 132, crowd 478 to 388 |
| Caustic Photons per Light and Sample<br>`eldr:causticsPhotons` | int 65536 | Photons each light sends towards caustic-casting objects per sample (1024 to 4M). More: less caustic noise per sample, but slower. [RT] | 7.4 ms per sample at 65536, 28 ms at 262144 (one sun) |
| Caustic Gather Radius<br>`eldr:causticsRadius` | float 0 (auto) | Radius, in scene units, over which the first sample gathers photons; later samples shrink it, so caustics sharpen as the image refines. Larger: smoother but blurrier at first. 0: 2% of the casting objects' size. [RT] | |
| Ray-Traced Refraction Sees Displacement<br>`eldr:rtRefractionDisplacement` | on | Displaced glass bends rays by its displaced shape (needs Ray Tracing Sees Displacement). Off: it refracts as if flat. [RT] | Displaced glass only |
| Ray-Traced Refraction Starts at Glass<br>`eldr:rtRefractionClip` | on | Traces refraction only from pixels that show glass, which is faster. The image is the same; off only to compare. [RT] | glass.usda 1080p: 41.7 to 37.5 ms per sample |

### Global Illumination

| Setting | Default | What it does | Cost |
|---|---|---|---|
| GI Probes along the Scene<br>`eldr:giProbes` | int 16 | GI probes along the scene's longest side (2-64, at most 16384 in all). More: catches smaller rooms and details, but costs time. [RT] | Linear in probes |
| GI Rays per Probe per Frame<br>`eldr:giRaysPerProbe` | int 128 | Rays each GI probe traces per frame (16-512). More: the bounced light settles faster and flickers less. [RT] | Linear |
| GI Hysteresis<br>`eldr:giHysteresis` | float 0.97 | How much each GI probe keeps of its earlier light per frame (0-0.99). Higher: smoother, slower to react to moving lights and objects. Little effect on stills. [RT] | |
| GI Preconverge<br>`eldr:giPreconverge` | on | Waits for new GI probes to settle before the image starts refining, so the first samples don't bake in dark, unsettled light. [film] [RT] | Cornell: RMSE 0.0128 to 0.0047 |
| GI Probe Relocation<br>`eldr:giRelocation` | on | Moves GI probes stuck inside or just behind walls out into open space, so light near walls doesn't leak or go dark. [RT] | emissive.usda (4096 probes) 2.50 to 3.04 ms |
| Ambient Occlusion Radius<br>`eldr:aoRadius` | float 1 | How far Ambient Occlusion looks for nearby surfaces, in scene units: 1 suits scenes in metres. Larger darkens wider areas. | |
| Ambient Occlusion Directions<br>`eldr:aoSlices` | int 3 | Directions Ambient Occlusion checks per pixel (1-8). More: less noise per frame; 3 is enough once the image refines. | Linear |
| SSGI Steps<br>`eldr:ssgiSteps` | int 16 | Steps SSGI takes along each direction (1-32). More: finds thinner and farther surfaces and brighter rooms, but slower. | Kitchen 1080p: 0.63 ms at 8, 0.83 ms at 16 |
| SSGI Radius<br>`eldr:ssgiRadius` | float 0 | How far SSGI gathers bounced light, in scene units. 0: across the whole screen, so far walls and ceilings light a room. A smaller radius keeps bounced light local. | |
| SSGI Resolution Divisor<br>`eldr:ssgiResolution` | int 2 | SSGI's resolution: 1 full, 2 half (denoised and upsampled, about twice as fast). | Full is about 2x |
| SSGI Tile Order<br>`eldr:ssgiTileOrder` | on | At half resolution, traces a different pixel of each 2x2 block every frame, so detail fills in over time. Off: always the same pixel. | Free |
| SSGI Visibility Bitmask<br>`eldr:ssgiBitmask` | off | Lets bounced light pass behind thin objects instead of treating them as solid walls. Use it for poles, railings and foliage. | Cornell 0.20 vs 0.14 ms |
| SSGI Bitmask Object Thickness<br>`eldr:ssgiBitmaskThickness` | float 0.25 | How thick SSGI assumes objects are, as a fraction of SSGI Radius, or at radius 0 of a tenth of the distance to the camera (needs SSGI Visibility Bitmask). Thicker blocks more light. | Free |
| Screen-Space Thickness Estimator<br>`eldr:ssgiThickness` | on | Estimates how thick objects are, so SSGI and SSR rays pass behind thin ones and handle grazing surfaces. Off: every surface counts as the same fixed thickness. | Noise |
| Screen-Space Back-Face Hits<br>`eldr:screenBackface` | on | An SSR or SSGI ray that hits the back of a surface keeps only Back-Face Hit Scale of its light, so less light leaks through objects. | SSR 0.27 ms on crowd; SSGI 0.60 to 0.76 ms |
| Screen-Space Back-Face Hit Scale<br>`eldr:screenBackfaceScale` | float 0.25 | How much light a back-face hit keeps (0-1): 0 treats surfaces as one-sided, 1 as two-sided. | Free |

### Lights

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Local Lights<br>`eldr:localLights` | on | Lights the scene with sphere, rect, disk and cylinder lights (spot lights included). Off: only distant and dome lights, for comparisons. | |
| Clustered Local Lights<br>`eldr:clusteredLights` | on | Sorts local lights into a 16x9x24 grid over the view, so each pixel shades only nearby lights (up to 255 per cell); much faster with many lights. Off: every pixel shades every light. | |
| Light Textures<br>`eldr:lightTextures` | on | Rect and disk lights take their colour from their texture (`inputs:texture:file`), both in the light they cast and on their visible shape, oriented as in Karma. | |
| Light Linking<br>`eldr:lightLinking` | on | Honours light and shadow links (`collection:lightLink`, `collection:shadowLink`, Houdini's Light Linker), up to 32 linked collections. Domes and fog ignore links. Off: every light lights and shadows everything. | Shadow-linked lights trace slower shadow rays |
| IES Profiles<br>`eldr:lightIes` | on | Shapes local lights with an IES profile (`inputs:shaping:ies:file`, with `angleScale` and `normalize`), oriented as in Karma. Off: lights shine evenly. | One texture read per light and pixel |
| Light Shapes Visible<br>`eldr:lightShapesVisible` | on | Shows light shapes to the camera, but only for lights with Render Light Geometry on (`inputs:karma:light:renderlightgeo` or `inputs:eldr:light:renderlightgeo`), as Karma. Needs Local Lights. | 0.12 ms |
| Dome in the Camera<br>`eldr:domeCameraVisible` | on | Shows the dome's image behind the scene. Off: the background is the clear colour (with its alpha), while the dome still lights and reflects. A dome's `inputs:karma:light:renderlightgeo = 0` and Hydra's `domeLightCameraVisibility` hide it too. | |
| Emissive Surfaces as Lights<br>`eldr:emissiveLights` | on | Small, bright, constant-coloured glowing objects also act as real lights (up to 32) that light their surroundings and cast shadows. Other glowing objects light the scene through GI only. | |
| Emissive Light Threshold<br>`eldr:emissiveLightThreshold` | float 4 | How bright a glowing object's emission must be (its brightest channel) to become a light. Lower it if a lamp shade lights nothing. | |
| Direct Light Intensity<br>`eldr:directIntensity` | float 1 | Scales direct light in the final image only, as a look tweak; bounced light and AOVs are unchanged. 1: as rendered. | None at 1. Other values add the light-split pass: crowd shading 14.8-16.5 vs 12.2-12.5 ms |
| Indirect Light Intensity<br>`eldr:indirectIntensity` | float 1 | Scales bounced light and reflections in the final image only, as a look tweak. 0: direct light only. 1: as rendered. | As above |

### Shadows

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Sun Shadows<br>`eldr:shadows` | on | Shadows from the sun (the first shadow-casting distant light), from a set of shadow maps that cover near to far (cascades). Off: the sun casts no shadow-map shadows. | |
| Sun Shadow Resolution<br>`eldr:shadowResolution` | int 2048 | Pixels per side of each sun shadow map (256-8192). Higher: sharper, steadier sun shadows; memory grows with the square. | Grows with the square |
| Sun Shadow Cascades<br>`eldr:shadowCascades` | int 4 | How many shadow maps split the sun's shadows from near to far (1-4). Fewer: faster, but blurrier shadows in the distance. | 1 cascade saves about 12 ms on crowd |
| Further Sun Shadow Maps<br>`eldr:extraSunShadowMaps` | on | Up to 2 more distant lights, such as a dome's extracted sun, get one hard-edged shadow map each over the view; surfaces, glass, volumes and fog use it. Further ones stay unshadowed. Off: they cast no shadows unless ray traced. | One shadow view per further sun, drawn once while the scene stands still; the 64 MB atlas if no local light has one. crowd_9k 2048x1080, 64 samples: 18 s either way, +60 MB |
| Local Light Shadows<br>`eldr:localShadows` | on | Shadow maps for sphere, rect, disk and spot lights, nearest lights first; lights that don't fit stay unshadowed. Off: local lights cast no shadow-map shadows. | Cached while still |
| Local Light Shadow Resolution<br>`eldr:localShadowResolution` | int 512 | Pixels per side of each local light's shadow map (64-2048). With many lights, maps halve in size (down to 64) so every shadowed light fits. | |
| Soft Shadows<br>`eldr:softShadows` | on | Soft shadow edges for local lights, from each light's size: sharp where objects touch, softer farther away. Off: a fixed, slight blur. | |
| Soft Sun Shadows<br>`eldr:softSunShadows` | on | Soft sun shadow edges from the sun's angle (`inputs:angle`): sharp where objects touch, softer farther away. Off: a fixed, slight blur. | Crowd 1.27 ms |
| Soft Shadow Samples<br>`eldr:softShadowSamples` | int 16 | Samples per soft shadow lookup (1-16). More: smoother soft edges per frame, but slower. | Linear |
| Jittered Light Shape per Sample<br>`eldr:shadowJitter` | on | Each sample renders the shadow maps from a different point on the light, so soft shadows refine to the exact result. Needs Soft Shadows. [film] | Sun error 0.016 vs 0.065 without; no frame-time change |
| Translucent Shadows<br>`eldr:translucentShadows` | on | Glass, water, tinted objects and thin-walled subsurface leaves cast coloured, lighter shadows, also into fog; volumes cast shadows the same way. Off: no coloured shadows. | 0.04 ms; 32 MB per map at default sun size |
| Translucent Sky Shadows<br>`eldr:translucentSkyShadows` | on | Glass balls and boxes tint the sky light under them by the share of the sky they hide, as ambient occlusion darkens it under opaque objects. Up to 8, nearest the camera. Needs Translucent Shadows. Off: sky light passes glass untinted. | |
| Translucent Shadow Resolution Divisor<br>`eldr:translucentShadowDivisor` | int 2 | Size of the coloured-shadow and volume-shadow maps as 1/N of the shadow maps (1, 2, 4). Higher: less memory, softer coloured shadows. | |
| Ray-Traced Shadow Rays per Light<br>`eldr:rtShadowSamples` | int 4 | Ray-traced shadow rays per light, pixel and sample (1-16). More: smoother soft shadows per sample. 1 is enough for finals, as the samples average out. [RT] | Linear |
| Ray-Traced Shadow Lights<br>`eldr:rtShadowLights` | int 4 | How many lights Ray-Traced Shadows traces (1-16): the sun and the most important local lights. The others keep shadow maps. [RT] | Up to a ray per light and pixel |
| Ray-Traced Shadow Denoise<br>`eldr:rtShadowDenoise` | on | Blurs ray-traced shadows across their soft edges only, so hard contact shadows stay hard; fades out over Effect Denoise Fade samples. Off: raw, noisier shadows. [RT] | One extra ray per light on shadowed pixels while active; no bias on the Kitchen |
| Ray-Traced Shadows through Glass and Volumes<br>`eldr:rtTranslucentShadows` | on | Ray-traced shadows pass through glass, taking its colour, and through volumes; so does the final gather's sky light. Off: those objects cast no ray-traced shadow. [RT] | Glass 1.08 vs 1.01 ms; volume 0.62 vs 0.49 ms |
| Ray-Traced Shadows of Further Suns<br>`eldr:rtExtraSunShadows` | on | While ray tracing runs for another effect, distant lights after the first, such as a dome's extracted sun, cast ray-traced shadows. Never starts ray tracing by itself. Off: they use Further Sun Shadow Maps. [RT] | volume.usda 720p: 3.4 ms on a GTX 1080 Ti |

### Reflections

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Screen-Space Reflections<br>`eldr:ssr` | on | Reflections of what is visible on screen, for floors, water and glossy objects; objects off screen are missing. Needs Post Processing. [prepass] | 6 ms on crowd |
| SSR Ray Steps<br>`eldr:ssrSteps` | int 32 | Steps per screen-space reflection ray (1-256). Raise it if long reflections break up. | Crowd shading 10.8 ms at 8, 11.7 ms at 32 |
| Denoised Glossy SSR<br>`eldr:ssrDenoise` | off | Blurry screen-space reflections on rough surfaces (up to roughness 0.8), filtered to remove noise. Off: sharp reflections that fade out as roughness grows. Not used with Ray-Traced Reflections. | Crowd shading 11.7 to 20.4 ms |
| SSR Hit Footprint Filter<br>`eldr:ssrHitFilter` | on | Rough screen-space reflections blur instead of sparkling, and fade out at the image edges. | 0.05 ms |
| SSR Back-Face Depth<br>`eldr:ssrBackfaceDepth` | on | SSR measures each object's thickness from a depth pass of its back faces, so a ball reflects round in a floor, without columns or streaks. Off: every surface is one pixel thick. Needs Screen-Space Thickness Estimator. [prepass] | 1080p: crowd 1.4 ms (as much as the depth prepass), Kitchen 0.06 ms, a ball on a floor 0.01 ms; one depth target (8 MB) while on |
| Ray-Traced Reflection Rays per Pixel<br>`eldr:rtReflectionSamples` | int 1 | Ray-traced reflection rays per pixel and sample (1-16). More: cleaner glossy reflections per sample; 1 is enough once the image refines. [RT] | Linear |
| Ray-Traced Reflection Denoise<br>`eldr:rtReflectionDenoise` | on | Slightly blurs rough ray-traced reflections to hide noise, fading out over Effect Denoise Fade samples. Off: raw rays. [RT] | About free |
| Ray-Traced Reflection Roulette<br>`eldr:rtReflectionRoulette` | on | Skips reflection rays where they add little (none on surfaces without specular, fewer on dim reflections), weighted so the finished image is the same. Faster. [RT] | 1080p final: crowd 249 to 146 ms per sample, Kitchen 49 to 34 |
| Reflection Probe<br>`eldr:reflectionProbe` | off | Captures the scene from one point into a cube map and uses it for reflections instead of the dome, so interiors reflect their room. Recaptured when the scene changes. Needs Dome Lighting. | Capture: Kitchen 6.5 ms, crowd 122 ms; then about 1 ms |
| Reflection Probe Resolution<br>`eldr:probeResolution` | int 128 | Size of each cube face of the Reflection Probe (16-512, power of two). Higher: sharper probe reflections. | CPU about 10 ms at 128 |
| Reflection Probe Position<br>`eldr:probePosition` | string empty | Where the Reflection Probe captures from, as "x y z". Empty: the centre of the scene's bounds. | |
| Authored Reflection Probes<br>`eldr:authoredProbes` | on | Meshes with `primvars:eldr:reflectionProbe` become reflection probes of their own (up to 8, smallest first), for several rooms. Needs Reflection Probe. | |
| Normalize Reflection Probes<br>`eldr:probeNormalize` | on | Matches a reflection probe's brightness to the local ambient light (with GI or SSGI on), up to 4x, so dark corners don't reflect a bright room. | |
| Planar Reflections<br>`eldr:planarReflections` | off | Flat glossy surfaces reflect a mirrored render of the scene: sharp, exact reflections for mirrors, floors and water. Marked by `primvars:eldr:planarReflection`, else the largest flat glossy surface. | One extra render per mirror in view |
| Planar Reflection Scale<br>`eldr:planarScale` | float 0.5 | Size of each mirror render as a fraction of the frame (0.1-1). Higher: sharper reflections, slower. | |
| Planar Reflection Mirrors<br>`eldr:planarCount` | int 4 | How many mirrors render per frame, largest first (up to 4). Each costs one extra render of the scene. | One render each |
| Planar Reflection Blur<br>`eldr:planarBlur` | float 1 | How much rough mirrors blur their reflection. 0: always sharp. | 12 taps on mirror pixels |
| Planar Reflections at Time Samples<br>`eldr:planarMotionBlur` | on | Mirrors render at each motion blur time sample, so reflections blur like the scene. Off: sharp reflections of moving things. [film] | None |

### Materials

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Karma Default Material<br>`eldr:defaultMaterialKarma` | on | Objects without a material shade as in Karma: 0.4 x `displayColor` (white without one) with a faint highlight, and `displayOpacity` as opacity. Off: `displayColor` at full strength and opaque, as Storm (about 2.3x as bright). | None measurable (a material like any other) |
| Normal Maps<br>`eldr:normalMaps` | on | Applies materials' normal maps for fine surface detail. Off: the mesh's own normals. | |
| Specular Workflow<br>`eldr:specularWorkflow` | on | UsdPreviewSurface materials with `useSpecularWorkflow = 1` use their `specularColor` for reflection strength. Off: the standard 0.04 reflectance. | |
| Clearcoat<br>`eldr:clearcoat` | on | A glossy coat over the base material, as on car paint: UsdPreviewSurface `clearcoat`, standard_surface `coat`, OpenPBR `coat_*`. Off: no coat. | Only materials that use it pay |
| Sheen<br>`eldr:sheen` | on | A soft, velvet-like shine at grazing angles, as on cloth: standard_surface `sheen`, OpenPBR `fuzz_*`. Off: no sheen. | Same |
| Thin Film Iridescence<br>`eldr:thinFilm` | on | Soap-bubble and oil-slick colours: standard_surface `thin_film_thickness`, OpenPBR `thin_film_*`. Off: no thin film. | Same |
| Anisotropic Specular<br>`eldr:anisotropicSpecular` | on | Stretched highlights, as on brushed metal (`specular_anisotropy`, `specular_rotation`, OpenPBR `specular_roughness_anisotropy`, glTF `anisotropy_strength`). Off: round highlights. | Same |
| Multiple Scattering<br>`eldr:multiScatter` | on | Keeps rough metals (standard_surface) and OpenPBR materials from darkening as roughness grows, as MaterialX and Karma do. Other specular stays single scattering, as Karma. Off: single scattering everywhere. | |
| Hair Shading<br>`eldr:hairShading` | on | Curves shade as hair, with hair highlights and no screen-space reflections. `primvars:eldr:hair = 0` opts a prim out. Off: curves shade as plain surfaces. | |
| Toon Shading<br>`eldr:toon` | on | Materials with `inputs:eldr:toonBands` shade in flat cartoon bands of light. Off: they shade normally. | |
| Toon Bands for Every Material<br>`eldr:toonBands` | int 0 (off) | Gives every material a cartoon look: light falls into this many flat bands per unit of light. 0: off (only materials that set their own bands). | |
| Textured Lobe Inputs<br>`eldr:texturedLobes` | on | Coat, sheen, anisotropy, thin film, specular and IOR inputs may be driven by textures or shading networks. Off: their constant values only. | Only such materials |
| MaterialX Graphs<br>`eldr:materialGraphs` | on | Runs MaterialX and UsdShade shading networks connected to material inputs. Off: plain values and textures only, which is faster. | Full screen about 2.5 ms at 1080p; Houdini's CarPaintMetallic 38 ms |
| Procedural Textures<br>`eldr:proceduralTextures` | on | Evaluates MaterialX noise nodes (noise, fractal, cell and Worley noise) in materials. Off: noise reads 0, which is faster. | fractal3d 0.74 ms, worleynoise3d 0.5 ms (full screen) |
| Subsurface Scattering<br>`eldr:subsurface` | on | Light spreading under the surface of skin, wax or marble, softening it, plus light shining through thin parts such as ears. Off: such materials shade as opaque. [prepass] | About 0.3 ms, only with subsurface materials |
| Subsurface Samples<br>`eldr:subsurfaceSamples` | int 15 | Samples of the subsurface blur per direction (1-64). Raise it if skin looks blotchy; Subsurface Quality multiplies it. | |
| Subsurface Profile<br>`eldr:subsurfaceProfile` | string burley | How far light spreads under the surface: `burley` (Burley's diffusion, as Karma: a soft glow reaching far past the radius) or `gaussian` (a tighter, harder falloff, for comparison). | Skin, jade and leaves at 1080p: burley 2.9 ms, gaussian 1.3 ms |
| Texture Anisotropic Filtering<br>`eldr:anisotropy` | int 16 | Keeps textures sharp on surfaces seen at a grazing angle, such as floors: 1 off, 16 sharpest. | Small |
| Texture Compression<br>`eldr:textureCompression` | off | Compresses 8-bit material textures on the GPU (cached on disk) to save memory, with a small quality loss. 16-bit, float and light textures stay exact. Off: exact textures, as Karma. | 4x less texture memory (gray: 2x); crowd_9k 172 to 112 MB; 52 dB PSNR vs exact |
| Textured Transmission and Subsurface Inputs<br>`eldr:texturedTransmission` | on | Transmission (colour, depth, thin walled) and subsurface (weight, colour, radius, scale) inputs may be driven by textures or shading networks. Off: their constant values only. | |
| Translucent Objects<br>`eldr:translucency` | on | Draws materials with opacity below 1 (glass, water, see-through objects). Off: such objects are not drawn. | |
| Order-Independent Transparency<br>`eldr:oit` | on | Blends overlapping transparent surfaces without sorting them, so layers of one mesh show in the right order. Off: whole meshes sorted back to front, which can misorder parts of one mesh. | 18 bytes per pixel, one pass |
| OIT Depth Weight<br>`eldr:oitDepthWeight` | float 4 | How strongly nearer transparent layers win over farther ones (0: plain average). Raise it if far layers show through near ones. | |
| Refraction<br>`eldr:refraction` | on | Glass and water bend and tint what is behind them, using what is visible on screen. Off: glass shows its background unbent. [prepass] | Glass scene 0.2 ms |
| Refraction Blur Samples<br>`eldr:refractionSamples` | int 8 | Blur samples for rough glass in screen-space refraction (1-16). Ray-traced refraction blurs as the image refines instead. | |
| Refraction Back Faces<br>`eldr:refractionBackFaces` | on | Draws the inside faces of glass and water (water seen from below, Snell's window, total internal reflection). Off: only front faces. | |
| Refraction Ray March<br>`eldr:refractionMarch` | on | Follows each bent ray across the screen to the surface it reaches, so a glass ball shows what is really behind it and water near the screen edge keeps showing the floor. Off: reads the point at the estimated thickness. | glass.usda 1080p: translucency pass 11.8-12.4 vs 11.7 ms off; full-screen water: no measurable change |
| Refraction through Layers<br>`eldr:multiRefraction` | on | Glass behind glass refracts correctly, layer by layer. Off: glass seen through other glass is missing from its refraction. | 0.1 ms per layer |
| Refraction Layers<br>`eldr:refractionLayers` | int 4 | The most glass layers handled per frame (1-16); further layers share the last. Volumes behind glass see up to 4. | Volumes under glass: 8 bytes per pixel per layer |
| Holdouts<br>`eldr:holdouts` | on | Matte (holdout) objects render black with zero alpha, so a plate can show through, but keep their depth, ids and shadows. Off: they render as normal objects. | None |
| Translucent Holdouts Cut Alpha<br>`eldr:holdoutAlpha` | on | A see-through holdout cuts colour and alpha by its opacity, for soft mattes. Off: see-through holdouts leave the image uncut. | Small pass when present |
| Opacity Mode Presence<br>`eldr:opacityMode` | on | UsdPreviewSurface `opacityMode` "presence" fades the whole surface, highlights included, with no refraction, as for cut-out leaves; "transparent" keeps highlights. Off: both act as transparent. | Free |

### Geometry

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Levels of Detail<br>`eldr:lod` | on | Meshes of 1000+ triangles get up to 3 simpler versions, and each copy draws the simplest one that looks the same (under about 1 pixel of error): faster crowds. Translucent and displaced meshes keep full detail. Off: full detail, for pixel-exact comparisons. | Crowd 68 to 28-34 ms; Kitchen at Very High 30 to 10.5 ms |
| Level of Detail Bias<br>`eldr:lodBias` | float 0 | Shifts which detail level Levels of Detail picks: +1 allows twice the error (faster), -1 half (more detail). Use negative values for finals. | -1: crowd 13.6M instead of 9.6M triangles |
| Level of Detail Crossfade<br>`eldr:lodFade` | on | Blends between detail levels over 8 frames instead of popping. Without TAA the blend shows as a dither pattern. | Crowd +0.05 ms |
| Subdivision Surfaces<br>`eldr:subdivision` | on | Smooths subdivision meshes (catmullClark, loop, bilinear) to Hydra's refine level (usdview's Complexity, Houdini's Level of Detail) or the mesh's `primvars:eldr:refineLevel`. Off: the coarse control cage. | 4x memory per level; Kitchen at Very High: 35M triangles, 5.5 s load |
| Subdivision Max Level<br>`eldr:maxRefineLevel` | int 3 | The highest subdivision level any mesh may use (0-8); each level has 4x the triangles and memory. | |
| Dicing by Screen Size<br>`eldr:screenDicing` | on | Picks each subdivision mesh's level from its size on screen, like Karma's dicing, so far meshes stay light. Quality from `primvars:eldr:object:dicingquality` (else Karma's, else 0.5). Off: the display level for all. | Kitchen in husk (960x540, level 3), off to on: 34.8M to 8.4M triangles, peak RAM 4.3 to 2.2 GB, load 11.9 to 3.2 s |
| Displacement Dicing Max Level<br>`eldr:displacementDicing` | int 7 | Dices displaced polygon meshes into a finer grid first, so displacement shapes the surface between corners, as Karma does. The highest level (0-8, each 4x the polygons); Dicing by Screen Size picks it. 0: off, the rest is bump shading. | displacement.usda 512x512, one quad to 32768 triangles at 7: per sample 0.75 to 1.1 ms (default), 9.0 to 13.2 ms (final). At most about 2M polygons per mesh |
| Displacement<br>`eldr:displacement` | on | Moves surface points by the material's displacement (UsdPreviewSurface, MaterialX). Needs enough vertices: use subdivision. Off: flat surfaces. | Only displaced materials pay |
| Displacement Scale<br>`eldr:displacementScale` | float 1 | Multiplies every material's displacement amount. 0: none; 1: as authored. | Rebuilds displaced ray-tracing data |
| Displacement Graphs<br>`eldr:displacementGraphs` | on | Allows displacement driven by shading networks (math, noise, several textures, primvars). Off: such materials don't displace. | Graph-displaced surfaces shade with flat facet normals; ray tracing builds extra data per mesh or instance |
| Curves as Camera-Facing Ribbons<br>`eldr:curveRibbons` | off | Draws curves without normals as flat ribbons facing the camera instead of round tubes: faster for grass and fur. `primvars:eldr:ribbons` (bool) overrides it per curve. | 2 triangles per step instead of 12; 100k blades 14.9 to 9.6 ms. Ray tracing sees lines. |
| Points as Camera-Facing Discs<br>`eldr:pointSprites` | off | Draws points as shaded discs facing the camera instead of small spheres: faster for large particle sets. `primvars:eldr:sprites` (bool) overrides it per prim. | 8 triangles per point instead of a sphere's 20 to 1280 (round as about a million triangles per prim allow). Ray tracing sees points. |

### Volumes and Fog

Fog is height fog set only here (USD has no fog schema). Volumes are OpenVDB `UsdVolVolume` prims and need a USD build with OpenVDB.

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Fog Density<br>`eldr:fogDensity` | float 0.02 | How thick the fog is at its base height, as extinction per scene unit. Higher: objects fade into fog sooner. | |
| Fog Height<br>`eldr:fogHeight` | float 0 | The height where the fog has its full density (Y, or Z with Fog Height along Z). | |
| Fog Height Falloff<br>`eldr:fogHeightFalloff` | float 0 | How fast the fog thins above its base height: density x exp(-falloff x height above base). 0: the same density at every height. | |
| Fog Anisotropy<br>`eldr:fogAnisotropy` | float 0.6 | Which way fog scatters light (-1 to 1): towards 1, bright glow and light shafts when looking towards lights; 0 even in all directions. | |
| Fog Ambient from GI Probes<br>`eldr:fogGiAmbient` | on | With GI on, fog takes its sky light from the GI probes, so closed rooms have no outdoor haze. Off: fog gets the dome's light everywhere, also indoors. | 0.2 to 0.47 ms with GI |
| Fog Color<br>`eldr:fogColor` | string "1 1 1" | Fog colour as "r g b": the share of light the fog scatters per channel. "1 1 1": white fog. | |
| Fog Max Distance<br>`eldr:fogMaxDistance` | float 0 | How far from the camera the fog grid reaches, in scene units. Shorter: finer fog detail near the camera. 0: to the far end of the scene. | |
| Fog Grid Size<br>`eldr:fogGridSize` | int 160 | Fog grid cells across the image (16-512), with 64 slices in depth. Higher: finer shafts and edges, slower. | 64: 0.06 ms; 256: 0.41 ms |
| Fog Height along Z<br>`eldr:fogZUp` | off | Measures fog height along Z instead of Y, for Z-up scenes. | |
| Fog Film Jitter<br>`eldr:fogFilmJitter` | on | Samples the fog at a random point in each grid cell and shifts the grid by part of a cell each sample, so the finished fog, even a narrow spot's beam, is smooth instead of blocky. [film] | 64 samples: RMSE 0.0014 to 0.00022 |
| Volume Step Size<br>`eldr:volumeStepSize` | float 1 | Distance between volume samples along a ray, in voxels. Larger: faster but coarser; smaller: finer detail in thin wisps. | |
| Volume Max Steps<br>`eldr:volumeMaxSteps` | int 256 | The most samples a ray takes through one volume box. Raise it if large volumes get cut off. | |
| Volume Shadow Steps<br>`eldr:volumeShadowSteps` | int 8 | Steps towards the light for each volume sample's shadow. More: more accurate self-shadowing, slower; Volume Quality multiplies it. | volume.usda 21 ms at 8, about 17 at 4, about 11 at 1 |
| Volume Local Shadow Stride<br>`eldr:volumeLocalShadowStride` | int 2 | Inside volumes, looks up local lights' shadows every this many steps (1-16) and reuses them in between: faster, nearly the same image. 1: every step. | volume + 3 sphere lights: 257.9 ms at 1, 237.3 at 2 (63 dB PSNR) |
| Volume Density Multiplier<br>`eldr:volumeDensity` | float 1 | Multiplies every volume's density: higher looks thicker and darker, lower more see-through. | |
| Volume Emission Multiplier<br>`eldr:volumeEmission` | float 1 | Multiplies every volume's emission, such as fire's glow. | |
| Volume Phase Anisotropy<br>`eldr:volumeAnisotropy` | float 0 | Which way volumes without a MaterialX volume material scatter light (-1 to 1): towards 1, brighter when backlit. 0: even in all directions, as Karma. | |
| Volume Resolution<br>`eldr:volumeResolution` | int 1 | Resolution of volume rendering: 1 full, 2 half (upsampled, about 3x faster, slightly softer edges). | volume.usda 22.0 ms at 1, 7.3 at 2 |
| Volumes under Transparency<br>`eldr:volumeUnderTransparency` | on | Smoke behind glass is dimmed by each pane in front of it and refracted (up to Refraction Layers), so it looks right through windows and bottles. | 5.8 vs 4.2 ms off |
| Volume Shadows<br>`eldr:volumeShadows` | on | Volumes cast sun shadows onto surfaces and fog. Off: volumes cast no shadows. | volume.usda 1.0 vs 0.07 ms |
| Local Volume Shadows<br>`eldr:localVolumeShadows` | on | Volumes also cast shadows from local lights. Needs Volume Shadows and Local Light Shadows. | Spot 0.17 ms, two sphere lights 0.8 ms |
| Volume Mutual Shadows<br>`eldr:volumeMutualShadows` | on | Volumes shadow each other, from the sun, local lights and the dome. Off: each volume only shadows itself. | Three volumes 35.2 to 39.2 ms; a lone volume is free |
| Volume Shadow Grid<br>`eldr:volumeShadowGrid` | int 64 | Precomputes each volume's self-shadowing on a grid with this many points per axis (8-128) instead of tracing it per ray: much faster. Higher: finer shadow detail, more memory. 0: trace every ray (slow). | volume + 3 lights, 1080p: 234 ms at 0, 77 at 64; up to 64 MB |
| Volume Default Fields<br>`eldr:volumeDefaultFields` | off | Volumes without a volume material also read Cd (colour), temperature (blackbody glow) and emission fields. Off: density only, as Karma. | |
| Volume Velocity Blur<br>`eldr:volumeVelocityBlur` | on | Volumes with Velocity Blur on (`primvars:eldr:` or `karma:object:vblur` above 0) blur along their `vel` field, as Karma. Off: sharp volumes. | Slower stepping while blurring |

### Camera and Film

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Temporal Anti-Aliasing<br>`eldr:taa` | on | Temporal anti-aliasing: jitters the camera each frame and blends frames for smooth edges. The film needs it too. Off: one jagged, aliased sample. | |
| Film Pixel Filter Radius<br>`eldr:filmFilterRadius` | float 1.5 | Width in pixels (0-2) of the filter that blends samples into pixels. Smaller: sharper but more aliased; larger: softer. [film] | |
| Object Motion Vectors<br>`eldr:objectMotion` | on | Tracks how objects move, for TAA and motion blur. Off: only camera motion counts, so moving objects smear in TAA and don't blur. | About 4 ms on crowd |
| Blend Shape Motion Vectors<br>`eldr:blendShapeMotion` | on | Changes in blend shape weights count as motion for skinned meshes, so faces blur and stay clean in TAA. | |
| Sampled Motion<br>`eldr:sampledMotion` | on | Takes motion from the scene's animation across the shutter, so a single frame blurs on its own. Off: motion since the last rendered frame. Rigid motion only. | Two extra transform reads per moving prim |
| Deformation Blur<br>`eldr:deformationBlur` | on | Deforming meshes, curves and points (caches, simulations, `velocities` with Velocity Blur) blur too. Skinned meshes blur either way. | Moving meshes: 16 bytes per vertex |
| Residual Motion Blur<br>`eldr:motionBlurResidual` | on | Adds a short blur between motion blur time samples, so fast motion looks smooth instead of like separate copies. [film] | Only while something moves over 1 pixel per sample |
| Lens Jitter per Sample<br>`eldr:dofJitter` | on | Each sample looks through a different point of the lens, for accurate depth of field and bokeh. Off: a post-process blur only. [film] | None |
| Residual DOF Blur<br>`eldr:dofResidualBlur` | on | Adds a little post blur on top of Lens Jitter per Sample, hiding gaps at low sample counts; it fades as samples grow. [film] | About 1.9 ms per sample at 1080p, with DOF |
| Lens Distortion<br>`eldr:lensDistortion` | on | Applies the camera's lens distortion (`lensDistortion:*` and OpenCV lens terms) to the finished image, AOVs included. | 0.32 ms at 1080p; none without distortion |
| Post Processing<br>`eldr:postProcess` | on | Runs the image steps after rendering: exposure, bloom, tonemapping, TAA, film, SSR, post DOF, motion blur, upscaling. Off: raw linear HDR, one aliased sample. For linear EXRs keep it on, with Tonemapper none, Auto Exposure off, Bloom 0. | |

## AOVs and Products

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Extra AOVs<br>`eldr:extraAovs` | on | Renders AOVs beyond colour, depth and ids when they are requested. Off: only those three. | None unless requested; a requested AOV disables `eldr:renderScale` |
| Unread AOVs Once Converged<br>`eldr:lazyExtraAovs` | on | AOVs that are requested but not displayed (Houdini requests normal and st) render once when the image is finished, not every frame. Saves time while you work. | macbeth 1866x737: 4.9 ms less per still frame |
| Cryptomatte Soft Edges and Ranks<br>`eldr:cryptomatteSoft` | on | Cryptomatte AOVs keep the 6 most-covering objects per pixel, with soft, anti-aliased edges, in Karma's layout. Off, and while moving: one object per pixel. [film] | 14 floats per pixel, only with a Cryptomatte AOV |
| Per-AOV Sample Filters<br>`eldr:aovFilters` | on | Each render var's `driver:parameters:aov:eldr:filter` (else Karma's `...karma:filter`) picks how samples combine: beauty, nearest, gauss, box, min, max, closest, farthest. Off: colour AOVs like the beauty, data AOVs nearest. [film] | None |
| Dome as Direct in Light Splits<br>`eldr:splitDomeDirect` | on | In light AOVs, counts unblocked dome light as direct light, as Karma. Off: all dome light counts as indirect. The beauty doesn't change. | Only with light AOVs |
| Write Render Products<br>`eldr:renderProducts` | on | Writes the render settings' EXR files once the frame is finished (husk, `usdrecord --renderSettingsPrimPath`). Off: writes nothing. | One readback per converged frame |
| Render Products at their Resolution<br>`eldr:productResolution` | on | Renders at the first image product's `resolution`, `pixelAspectRatio` and `dataWindowNDC` (2048x1080 without one, as husk). Off: at the window's size. | One CPU resample per frame while sizes differ |

## Performance and Debug

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Depth Prepass<br>`eldr:depthPrepass` | on | Draws depth first so each pixel shades once. AO, SSR, SSGI, refraction, subsurface and ray tracing need it. Turn off only for comparisons. | Off saves 14 ms on crowd but drops those effects |
| Frustum Culling<br>`eldr:frustumCulling` | on | Skips objects outside the camera's view, which is faster. The image is the same; off only to compare. | |
| Small Object Culling<br>`eldr:cullPixels` | float 0.5 | Skips object copies smaller than this many pixels in radius on screen, which speeds up crowds and scattered props. 0: off; use 0 for finals with tiny props. | |
| Shadow Small Object Culling<br>`eldr:shadowCullPixels` | float 0.5 | Sun shadows skip object copies smaller than this many shadow-map pixels in radius, so thousands of distant agents drop out of far shadows. 0: off. | |
| Offline Samples per Execute<br>`eldr:offlineBatchSeconds` | float 1 | husk and usdrecord render samples back to back for up to this many seconds before reporting progress, which is faster. 0: one sample at a time. | husk crowd 9k 480x270: 13.3 s at 0, 6.9 s at 1 |
| Share Frames with OpenGL<br>`eldr:shareFrame` | on | usdview and usdrecord show the image straight from the GPU, without a copy through the CPU: faster display. Falls back to a copy when unsupported. | 1080p 17.4 to 3.2 ms; 4K 67 to 9.6 ms |
| Overlap Readback while Moving<br>`eldr:overlapReadback` | on | In Houdini, while the camera moves, shows each frame one frame late so the GPU and Houdini work at the same time: a faster viewport. A still camera's frame is never late. | step4 1866x737: 20.3 to 14.0 ms a frame |
| Read Back in Place<br>`eldr:inPlaceReadback` | on | In Houdini, hands the image over without an extra copy: a faster viewport. Off only to compare. | macbeth 1866x737, 4 AOVs: 14.6 to 8.2 ms a frame |
| Keep Ids while Moving<br>`eldr:staleMovingIds` | on | In Houdini, keeps the last still frame's object ids (used for picking and the selection outline) while the camera moves, instead of reading them every frame: a faster viewport. | macbeth in Houdini 22: 90 to 104 fps |
| Pipeline Cache on Disk<br>`eldr:pipelineCache` | on | Saves compiled shaders to `%LOCALAPPDATA%\Eldr\pipelines-*.bin` (`HDVK_PIPELINE_CACHE_DIR` moves it), so later sessions skip most shader compiling and start much faster. | Crowd first frame 33.5 s cold, 2.0 s cached; file about 16 MB |
| Prewarm Pipelines<br>`eldr:pipelinePrewarm` | on in Hydra, off for embedders | Compiles shaders for likely toggles (GI, SSGI, fog) in the background, so turning them on doesn't freeze the viewport. Off in husk and usdrecord. | Half the CPU cores in the background |
| Compile Pipelines on Demand<br>`eldr:lazyPipelines` | on | Compiles only the shaders the scene uses, when first needed: much faster first frame. Off: compiles every variant up front. | macbeth, cold cache: first frame 34.3 s off, 6.1 s on |
