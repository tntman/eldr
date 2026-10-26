# Eldr render settings

Every setting is a Hydra render setting `eldr:<name>`. Unset settings have the default shown.

| Where | How |
|---|---|
| usdview | **Eldr > Render Settings**: grouped, filterable, changed settings in bold, one reset button |
| Python | `engine.SetRendererSetting("eldr:ssr", False)` |
| USD | an `eldr:<name>` attribute on the active UsdRenderSettings prim (typed like the default) |

**Final render recipe:** keep the defaults, turn on `eldr:finalQuality`, raise `eldr:filmSamples`; optionally turn on GI, SSGI, ray-traced shadows and reflections.

## Reading the tables

| Mark | Meaning |
|---|---|
| flag, int, float, string | Type; ranges are clamped |
| [film] | Acts only under film accumulation or final quality |
| [interactive] | Acts only while the camera or scene moves |
| [RT] | Needs a ray-tracing GPU; ignored without (`HDVK_NO_RAY_TRACING=1` pretends there is none) |
| [prepass] | Needs `eldr:depthPrepass` |
| Cost | GPU ms, GTX 1080 Ti, 1280x720. Crowd = 20,000 skinned agents (21 ms at defaults). Kitchen = Pixar Kitchen_set. Under 1 ms is noise. |

Optional shader code compiles out when its setting is off. The first frame using a new combination compiles a pipeline set (seconds, unless cached).

The settings people change most come first and are open in the UIs; rarely touched ones are folded (Houdini) and last. Same order as `GROUPS`.

## Final render

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Final Quality<br>`eldr:finalQuality` | off | [film] Offline mode: every change restarts the film from scratch, never from TAA. Also on when Hydra's `enableInteractive` is false (husk). | |
| Film Samples<br>`eldr:filmSamples` | int 64 | [film] Samples until converged. 256+ for noise-free finals. | Linear |
| Adaptive Sampling Threshold, Final Quality<br>`eldr:adaptiveFinalThreshold` | float 0 (off) | [film] Opt-in noise threshold for final quality, e.g. 0.01; off, a final frame takes every film sample. Pixels touched only now and then can stop too early. | Fewer samples |
| Final Quality Shadows<br>`eldr:finalShadows` | string maps | [RT][film] `maps`, `raytraced` or `auto`: how final quality shadows lights when `eldr:rtShadows` is off. `raytraced` traces the sun and 15 lights; `auto` traces when the scene is dense enough. Interactive follows `eldr:rtShadows`. | 960x540, 64 samples, traced / maps: City 2.2 / 4.5 s; crowd 0.8 / 1.4 s; Kitchen 0.93 / 0.64 s |
| Ray-Traced Refraction<br>`eldr:rtRefraction` | int 1 | [RT][prepass] 0 off, 1 final quality, 2 always. Traces the nearest refracting surface's ray through the glass (back faces, glass behind glass, total internal reflection, absorption), shaded at the hit; rough glass jitters per sample. | glass.usda 8.1-9.7 ms (compute fallback); crowd unchanged |
| Motion Blur<br>`eldr:motionBlur` | on | Blur for an open shutter (`shutter:open`, `shutter:close`). Interactive: post blur. | Post blur 0.2 ms |
| Motion Blur Time Samples<br>`eldr:motionBlurTimeSamples` | on | [film] Each sample renders the whole scene at a shutter time (shadows, rays, refraction included; instances, joints, blend shapes, camera). Off: post blur. | Crowd 1.7-2.1 ms per sample |
| Depth of Field<br>`eldr:depthOfField` | on | Thin-lens blur from camera `fStop`, `focalLength`, `focusDistance` (perspective cameras; not usdview's free camera). | Post DOF about 1.9 ms at 1080p |

## Lighting and GI

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Global Illumination (RTX probes)<br>`eldr:gi` | off | [RT] DDGI probe grid: diffuse light is the scene's bounced light. For interiors and emissive lighting. | Kitchen 2-3 ms (1728 probes x 128 rays); crowd 0.2 ms; plus acceleration structure memory |
| Screen-Space GI (replaces AO)<br>`eldr:ssgi` | off | [prepass] Per-pixel indirect diffuse gathered from the screen; close-range colour bleeding. No ray tracing needed. | Half res 0.67 ms, full 1.55 ms (Kitchen) |
| Ray-Traced Shadows<br>`eldr:rtShadows` | off | [RT][prepass] Traced shadows for the sun and the 3 most important local lights (15 in final quality), instead of maps. Soft from light size; the film averages the noise. | Kitchen about 10 ms |
| Ray-Traced Reflections<br>`eldr:rtReflections` | off | [RT][prepass] Traced reflections replace dome and SSR up to roughness 0.7; shaded at hits. Sees off-screen geometry. | Kitchen about 15 ms |
| Ambient Occlusion (GTAO)<br>`eldr:ambientOcclusion` | on | AO on the dome's diffuse and specular light. [prepass] Replaced by SSGI when that is on. | 0.34 ms |
| Dome Light Lighting (IBL)<br>`eldr:imageBasedLighting` | on | Dome diffuse (SH9) and specular (prefiltered cube). Off: direct lights only. Also gates the reflection probe. | Small |
| Dome Sun Extraction<br>`eldr:domeSun` | on | HDRI texels above the threshold become a shadowed distant light. Off: HDRI only. | CPU work on dome change |
| Volumetric Fog<br>`eldr:fog` | off | Height fog lit by all lights, over surfaces and sky, with god rays. | 0.19 ms on crowd; shading +0.5 ms |
| Volumes (OpenVDB)<br>`eldr:volumes` | on | Ray-marches OpenVDB volumes (NanoVDB on GPU). Only in builds against OpenVDB USD. | See below |

## Image

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Auto Exposure<br>`eldr:autoExposure` | on | Histogram exposure that adapts over time. Off for deterministic finals; set `eldr:exposureCompensation`. | 0.11 ms |
| Exposure Compensation<br>`eldr:exposureCompensation` | float 0 | EV added; fixed exposure when auto is off. | |
| Tonemapper<br>`eldr:tonemapper` | string aces | `aces`, `agx`, `agxPunchy` or `none` (exposed linear, unclamped). | |
| Bloom Intensity<br>`eldr:bloomIntensity` | float 0.04 | Bloom strength (0 = off). | |
| Linear Beauty<br>`eldr:productLinear` | on | Product beauty and the Houdini viewport's colour are linear scene colour, the frame's exposure, no bloom or tonemap. husk and the viewport skip auto exposure unless the settings prim asks; Houdini applies its own view. Off: Eldr's tonemapped colour. | One extra pass |

## Advanced

Quality knobs (steps, radii, resolutions, thresholds) and the other feature toggles.

### Geometry and products

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Levels of Detail<br>`eldr:lod` | on | Meshes of 1000+ triangles get up to 3 coarser levels, chosen per instance on the GPU (error under about 1 px). Built on worker threads; not converged until they exist. Translucent and displaced meshes keep full detail. Off for pixel-exact comparison. | Crowd 68 to 28-34 ms; Kitchen at Very High 30 to 10.5 ms |
| Level of Detail Bias<br>`eldr:lodBias` | float 0 | +1 allows twice the error, -1 half. Use negative for finals. | -1: crowd 13.6M instead of 9.6M triangles |
| Level of Detail Crossfade<br>`eldr:lodFade` | on | A level change dithers over 8 frames (TAA or film blends it). Off: levels pop. Without TAA the dither shows as stipple. | Crowd +0.05 ms |
| Subdivision Surfaces<br>`eldr:subdivision` | on | Refines catmullClark, loop and bilinear meshes (OpenSubdiv) to Hydra's refine level (usdview Complexity, `usdrecord --complexity`, Houdini's Level of Detail) or a mesh's `primvars:eldr:refineLevel`. Off draws the cage. | 4x memory per level; Kitchen at Very High: 35M triangles, 5.5 s load |
| Subdivision Max Level<br>`eldr:maxRefineLevel` | int 3 | Clamps the refine level (0-8). | |
| Displacement<br>`eldr:displacement` | on | Honours material displacement (UsdPreviewSurface, MaterialX) in the vertex stage, so every pass sees it. Needs enough vertices: use subdivision. | Only displaced materials pay |
| Curves as Camera-Facing Ribbons<br>`eldr:curveRibbons` | off | Curves without normals draw as flat ribbons facing the eye, not 6-sided tubes. `primvars:eldr:ribbons` (bool) overrides per curve. | 2 triangles per step instead of 12; 100k blades 14.9 to 9.6 ms. Ray tracing sees lines. |
| Points as Camera-Facing Discs<br>`eldr:pointSprites` | off | Points draw as shaded discs instead of icosahedra. `primvars:eldr:sprites` (bool) overrides per prim. | 8 triangles per point instead of 20. Ray tracing sees points. |
| Displacement Scale<br>`eldr:displacementScale` | float 1 | Multiplies every displacement offset. | Rebuilds displaced BLASes |
| Displacement Graphs<br>`eldr:displacementGraphs` | on | Runs shading graphs (math, noise, several textures, primvars) per vertex for displacement. Off: such materials don't displace. | Graph-displaced surfaces shade with facet normals. Ray tracing builds BLASes per mesh, or per instance (256 largest on screen) for graphs reading world position, normal or instance primvars. |
| Texture Anisotropic Filtering<br>`eldr:anisotropy` | int 16 | Filter level (1 = off, 16 max). | Small |
| Light Splits: Dome as Direct (Karma)<br>`eldr:splitDomeDirect` | on | Dome light unblocked by AO/SSGI counts as direct, as in Karma. Off: all dome light is indirect. Beauty unchanged. | Only with light splits |
| Cryptomatte: Soft Edges and Ranks (film samples)<br>`eldr:cryptomatteSoft` | on | [film] Keeps the 6 heaviest ids per pixel with coverages, as layers `<var>00..02` (Karma layout); soft edges. Off or interactive: one id at coverage 1. | 14 floats per pixel, only with a Cryptomatte AOV |
| Direct Light Intensity (final pixel only)<br>`eldr:directIntensity` | float 1 | Multiplies direct light (lights and unblocked dome) in the final pixel only; GI still bounces unscaled light. Emission and AOVs untouched. | None at 1. Other values compile the light splits: lights scene 0.92 vs 0.62 ms; crowd shading 14.8-16.5 vs 12.2-12.5 ms with SSR or refraction. |
| Indirect Light Intensity (final pixel only)<br>`eldr:indirectIntensity` | float 1 | Same for GI, SSGI, probes, reflections. 0 leaves direct light only. | As above |
| Write Render Products<br>`eldr:renderProducts` | on | Writes the active UsdRenderSettings' EXR products when the frame converged (`usdrecord --renderSettingsPrimPath`, husk). Off writes nothing. | One readback per converged frame |
| Render Products at their Resolution<br>`eldr:productResolution` | on | Renders at the first raster product's `resolution`, `pixelAspectRatio` and `dataWindowNDC`. No authored `resolution` means 2048x1080 (as husk). The app's own buffers get a nearest resample; extra AOVs stay empty. Off: frame size. | One CPU resample per frame while sizes differ |

### Lighting

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Local Lights<br>`eldr:localLights` | on | Sphere, rect, disk, cylinder lights. | |
| Clustered Local Lights<br>`eldr:clusteredLights` | on | 16x9x24 cluster grid, 63 lights per cluster. Off loops all lights per pixel. | |
| Light Textures<br>`eldr:lightTextures` | on | Rect and disk lights multiply radiance by `inputs:texture:file` (Karma orientation). | |
| Light Shapes Visible<br>`eldr:lightShapesVisible` | off | Draws sphere, rect, disk and cylinder shapes to the camera. `bool inputs:karma:light:renderlightgeo = 0` hides one light. No point or line lights, shaping or textures. Needs `eldr:localLights`. | One pass, 0.12 ms |
| Dome in the Camera (background)<br>`eldr:domeCameraVisible` | on | Draws the dome's texture behind the scene. Off: the clear colour (with its alpha) shows; lighting and reflections still use the dome. `bool inputs:karma:light:renderlightgeo = 0` hides one dome; Hydra's `domeLightCameraVisibility` also applies. | |
| Emissive Surfaces as Lights<br>`eldr:emissiveLights` | on | Small bright constant emitters also become rect or sphere lights with shadows (max 32). Others light via GI only. | |
| Emissive Light Threshold<br>`eldr:emissiveLightThreshold` | float 4 | Brightest-channel radiance above which an emitter becomes a light. | |
| Dome Sun Threshold<br>`eldr:domeSunThreshold` | float 10 | Luminance counting as sun. Lower if the sun is missed. | |
| Ambient Occlusion Radius<br>`eldr:aoRadius` | float 0.5 | World-space scan radius. Match scene scale. | |
| Ambient Occlusion Directions<br>`eldr:aoSlices` | int 3 | Slices per pixel (1-8). 3 is enough with the film. | Linear |
| GI Probes along the Scene<br>`eldr:giProbes` | int 16 | Probes along the longest side (2-64, max 16384 total). | Linear in probes |
| GI Rays per Probe per Frame<br>`eldr:giRaysPerProbe` | int 128 | 16-512. More is faster and cleaner. | Linear |
| GI Hysteresis<br>`eldr:giHysteresis` | float 0.97 | History weight (0-0.99). Higher is smoother, slower to react. Little effect on stills. | |
| GI Preconverge<br>`eldr:giPreconverge` | on | [film] Waits for fresh probes to settle before averaging. | Cornell: RMSE 0.0128 to 0.0047 |
| SSGI Steps<br>`eldr:ssgiSteps` | int 8 | Steps per direction and side (1-32). | |
| SSGI Radius<br>`eldr:ssgiRadius` | float 1 | World-space gather radius; use a fraction of the room size. | |
| SSGI Resolution Divisor<br>`eldr:ssgiResolution` | int 2 | 1 = full, 2 = half (denoised, depth-aware upsample). | Full is about 2x |
| SSGI Tile Order<br>`eldr:ssgiTileOrder` | on | At half res, each frame traces a different pixel of every 2x2 tile (Morton order). Off: top-left only. | Free |
| Screen-Space Thickness Estimator<br>`eldr:ssgiThickness` | on | SSGI and SSR rays respect thin features and grazing surfaces. Off steps evenly (5% of distance thick). | Noise |
| SSGI Visibility Bitmask<br>`eldr:ssgiBitmask` | off | 32-sector bitmask instead of highest horizon: light passes behind thin objects. Use for poles, railings, foliage. | Cornell 0.20 vs 0.14 ms |
| SSGI Bitmask Object Thickness<br>`eldr:ssgiBitmaskThickness` | float 0.25 | Occluder thickness as a fraction of `eldr:ssgiRadius`; thicker hides more light. | Free |
| Screen-Space Back-Face Hits<br>`eldr:screenBackface` | on | An SSR or SSGI ray hitting a surface from behind takes `eldr:screenBackfaceScale` of its light; less leaking through objects. | SSR 0.27 ms on crowd; SSGI horizon 0.60 to 0.76 ms; bitmask free |
| Screen-Space Back-Face Hit Scale<br>`eldr:screenBackfaceScale` | float 0.25 | Light share of a back-face hit (0-1): 0 one-sided, 1 double-sided. | Free |
| Direct Light Clamp<br>`eldr:directClamp` | float 0 (off) | Caps a pixel's direct light brightness, keeping hue. Firefly killer; costs energy. | |
| Indirect Light Clamp<br>`eldr:indirectClamp` | float 10 (0 = off) | Same for every bounce (SSGI, SSR and traced hits, GI rays). Dome not clamped. | |

### Shadows

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Sun Shadows (cascades)<br>`eldr:shadows` | on | Cascaded shadow maps for the first shadow-casting distant light. | |
| Sun Shadow Resolution<br>`eldr:shadowResolution` | int 2048 | Texels per cascade side (256-8192). | Grows with the square |
| Sun Shadow Cascades<br>`eldr:shadowCascades` | int 4 | 1-4. | 1 cascade saves about 12 ms on crowd, spreads resolution thin |
| Local Light Shadows<br>`eldr:localShadows` | on | 4096x4096 atlas for sphere, rect, disk, spot lights (spot up to 80 degrees: one tile; others six cube faces). Nearest lights first; the rest unshadowed. | Cached while still |
| Local Light Shadow Resolution<br>`eldr:localShadowResolution` | int 512 | Texels per tile (64-2048). Tiles halve (to 64) so every shadowed light fits. | |
| Soft Shadows (PCSS)<br>`eldr:softShadows` | on | Contact-hardening penumbrae for local lights from their size. Off: 3x3 PCF. | |
| Soft Sun Shadows<br>`eldr:softSunShadows` | on | Same for the sun from `inputs:angle`. No angle: PCF. | Crowd 1.27 ms |
| Soft Shadow Samples<br>`eldr:softShadowSamples` | int 16 | Blocker search and filter taps (1-16). | Linear |
| Translucent Shadows<br>`eldr:translucentShadows` | on | Glass, water and tinted meshes tint shadows, also in fog. Volumes cast into the same maps. | 0.04 ms; maps 32 MB each at default sun size |
| Translucent Shadow Resolution<br>`eldr:translucentShadowDivisor` | int 2 | Tint map size as 1/N of the shadow maps (1, 2, 4). Also sets volume shadow resolution. | |
| Ray-Traced Shadow Rays per Light<br>`eldr:rtShadowSamples` | int 4 | Rays per light and pixel (1-16), interactive. Final quality: 1 per light and film sample. | Linear |
| Ray-Traced Skinned Instances<br>`eldr:rtSkinnedInstances` | int 16 | [RT] Largest skinned instances (0-256) that block rays for all ray-traced effects. | Kitchen 0.45 ms (16), 0.69 ms (90); crowd 0.68 (16), 2.7 ms (256) |
| Ray Tracing Sees Displacement<br>`eldr:rtDisplacement` | on | [RT] BLASes (skinned too) use displaced vertices. | Rebuilds on scale change |
| Jittered Light Shape per Sample<br>`eldr:shadowJitter` | on | [film, final quality] Each sample renders maps from a point on the light: exact penumbra. Needs soft shadows. Interactive films keep PCSS. | Sun error 0.016 vs PCSS 0.065; no frame-time change |
| Ray-Traced Shadows through Glass and Volumes<br>`eldr:rtTranslucentShadows` | on | [RT] Traced rays pass translucent meshes (taking their colour) and march volumes. Off: they cast no traced shadows. | Glass 1.08 vs 1.01 ms; volume 0.62 vs 0.49 ms |

### Reflections

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Screen Space Reflections<br>`eldr:ssr` | on | Rays against the depth buffer, shaded from last frame. [prepass] Needs post processing. | 6 ms on crowd |
| Reflection Ray Steps<br>`eldr:ssrSteps` | int 32 | 1-256. Raise if long reflections break up. | Crowd shading 10.8 ms at 8, 11.7 ms at 32 |
| Denoised Glossy SSR<br>`eldr:ssrDenoise` | off | One GGX ray per pixel with spatial, temporal and bilateral filters; glossy up to roughness 0.8. Off: mirror ray fading out at roughness 0.3-0.6. Not with ray-traced reflections. | Denoiser 1.1 ms; crowd shading 11.7 to 20.4 ms |
| SSR Hit Footprint Filter<br>`eldr:ssrHitFilter` | on | Reads hits at the mip matching the ray cone: rough reflections blur instead of sparkle; hits fade at image edges. | 0.05 ms |
| Ray-Traced Reflection Rays per Pixel<br>`eldr:rtReflectionSamples` | int 1 | 1-16. The film makes 1 enough. | Linear |
| Reflection Probe<br>`eldr:reflectionProbe` | off | One captured cube (centre or `eldr:probePosition`), parallax corrected, replaces the dome's specular. Recaptured on light, material, dome or geometry change. Follows `eldr:imageBasedLighting`. | Capture once: Kitchen 6.5 ms, crowd 122 ms; lookup about 1 ms |
| Reflection Probe Resolution<br>`eldr:probeResolution` | int 128 | Cube face size (16-512, power of two). | CPU prefilter about 10 ms at 128 |
| Reflection Probe Position<br>`eldr:probePosition` | string empty | "x y z" capture point; empty = scene bounds centre. | |
| Authored Reflection Probes<br>`eldr:authoredProbes` | on | Meshes with `primvars:eldr:reflectionProbe` are probes (max 8, smallest first). Needs `eldr:reflectionProbe`. | |
| Normalize Reflection Probes<br>`eldr:probeNormalize` | on | Scales a probe by local ambient light over captured ambient (GI or SSGI on), clamped 4x. | |
| Planar Reflections<br>`eldr:planarReflections` | off | Flat glossy mirrors (`primvars:eldr:planarReflection`, else the largest flat glossy opaque mesh) reflect a mirrored render. | Each mirror in view is one extra render; crowd scene 0.1 ms |
| Planar Reflection Scale<br>`eldr:planarScale` | float 0.5 | Mirror render size as a fraction of the frame (0.1-1). | |
| Planar Reflection Mirrors<br>`eldr:planarCount` | int 4 | Mirrors per frame, largest first (max 4). | One render each |

### Materials

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Normal Maps<br>`eldr:normalMaps` | on | Applies normal maps. | |
| Specular Workflow<br>`eldr:specularWorkflow` | on | UsdPreviewSurface `useSpecularWorkflow = 1` uses F0 = `specularColor`. Off: 0.04. | |
| Clearcoat<br>`eldr:clearcoat` | on | Second GGX lobe: UsdPreviewSurface `clearcoat`, standard_surface `coat`, OpenPBR `coat_*`. | Only materials with lobes pay |
| Sheen<br>`eldr:sheen` | on | Charlie lobe: standard_surface `sheen`, OpenPBR `fuzz_*`. | Same |
| Thin Film Iridescence<br>`eldr:thinFilm` | on | standard_surface `thin_film_thickness`, OpenPBR `thin_film_*`. | Same |
| Anisotropic Specular<br>`eldr:anisotropicSpecular` | on | Stretches highlights along u (`specular_anisotropy`, `specular_rotation`, OpenPBR `specular_roughness_anisotropy`). No uvs: brushed around world up. | Same |
| Multiple Scattering<br>`eldr:multiScatter` | on | GGX energy compensation (Fdez-Aguera, Filament) so rough metals keep their energy. Off: single scattering as Karma's UsdPreviewSurface (darker when rough). | |
| Hair Shading<br>`eldr:hairShading` | on | BasisCurves shade as hair (Kajiya-Kay), no SSR. `primvars:eldr:hair = 0` opts a prim out. | |
| Toon Shading<br>`eldr:toon` | on | Materials with `inputs:eldr:toonBands` quantise lighting into flat bands. | |
| Toon Bands for Every Material<br>`eldr:toonBands` | int 0 (off) | Bands per unit of light for all materials. Whole-scene NPR look. | |
| Textured Lobe Inputs<br>`eldr:texturedLobes` | on | Coat, sheen, anisotropy, thin film, specularColor, IOR inputs may be textures or graphs. Off reads constants. | Only such materials |
| MaterialX Graphs<br>`eldr:materialGraphs` | on | Compiles MaterialX/UsdShade graphs behind inputs into per-pixel programs. Off: plain inputs and textures. | Full-screen graph about 2.5 ms at 1080p; Houdini's Ceramic 3.5 ms, CarPaintMetallic (205 ops, cell noise) 38 ms |
| Procedural Textures<br>`eldr:proceduralTextures` | on | MaterialX noise nodes in programs. Off: noise reads 0. | fractal3d 0.74 ms; worleynoise3d 0.5 ms (full screen) |
| Subsurface Scattering<br>`eldr:subsurface` | on | [prepass] Screen-space per-channel Gaussian blur of diffuse light, plus sun through thin parts. | About 0.3 ms, only with subsurface materials |
| Subsurface Samples<br>`eldr:subsurfaceSamples` | int 15 | Taps per direction (1-64). | |

### Transparency

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Textured Transmission and Subsurface Inputs<br>`eldr:texturedTransmission` | on | Transmission, colour, depth, thin walled, subsurface weight, colour, radius and scale may be textures or graphs. Off reads constants. | |
| Translucent Objects<br>`eldr:translucency` | on | Draws materials with opacity below 1. Off: not drawn. | |
| Order-Independent Transparency<br>`eldr:oit` | on | Weighted blended OIT. Off sorts meshes back to front (can misorder one mesh's triangles). | 18 bytes per pixel, one pass |
| OIT Depth Weight<br>`eldr:oitDepthWeight` | float 4 | Distance weight exponent (0 = plain average). Raise if far layers show through near ones. | |
| Refraction<br>`eldr:refraction` | on | [prepass] Materials with transmission or `ior` bend and tint the opaque colour behind them. Screen space: off-screen content isn't refracted. | Glass scene 0.2 ms |
| Refraction Blur Samples<br>`eldr:refractionSamples` | int 8 | Taps for rough refraction (1-16). | |
| Holdouts<br>`eldr:holdouts` | on | Matte objects draw black, zero alpha, keep depth, ids and shadows. | None |
| Refraction Back Faces<br>`eldr:refractionBackFaces` | on | Draws back faces (water from below, Snell's window, total internal reflection). Off discards them. | |
| Refraction Through Layers<br>`eldr:multiRefraction` | on | Glass behind glass: background re-copied after each layer. | 0.1 ms per copy |
| Refraction Layers<br>`eldr:refractionLayers` | int 4 | Most copies per frame (1-16); more layers share the last. | |
| Ray-Traced Refraction Interfaces<br>`eldr:rtRefractionBounces` | int 4 | Surfaces a ray passes (1-16): 2 for one ball, 4 for a ball behind a pane. | |
| Ray-Traced Refraction Sees Displacement<br>`eldr:rtRefractionDisplacement` | on | [RT] Displaced glass bends rays by its displaced triangles and height-map bump (with `eldr:rtDisplacement`). Off: flat glass look. | Displaced glass only |
| Translucent Holdouts Cut Alpha<br>`eldr:holdoutAlpha` | on | A translucent holdout cuts colour and alpha by its opacity. | Small pass when present |

### Fog and volumes

Fog is height fog in a froxel grid, set only here (USD has no fog schema). Volumes are OpenVDB `UsdVolVolume` prims (USD built with OpenVDB only).

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Fog Density<br>`eldr:fogDensity` | float 0.02 | Extinction per scene unit at base height. | |
| Fog Height<br>`eldr:fogHeight` | float 0 | Base height (Y, or Z with `eldr:fogZUp`). | |
| Fog Height Falloff<br>`eldr:fogHeightFalloff` | float 0 | Density x exp(-falloff x (height - base)); 0 = uniform. | |
| Fog Anisotropy<br>`eldr:fogAnisotropy` | float 0.6 | Henyey-Greenstein g (-1..1); towards 1: shafts toward lights. | |
| Fog Ambient from GI Probes<br>`eldr:fogGiAmbient` | on | With GI, dome light in fog comes from probes (no outdoor haze in closed rooms). | 0.2 to 0.47 ms with GI |
| Fog Color<br>`eldr:fogColor` | string "1 1 1" | Scattering albedo "r g b". | |
| Fog Max Distance<br>`eldr:fogMaxDistance` | float 0 | Froxel depth; 0 = scene far end. Shorten for detail near the camera. | |
| Fog Grid Size<br>`eldr:fogGridSize` | int 160 | Froxels across (16-512), 64 depth slices. | 64: 0.06 ms; 256: 0.41 ms |
| Fog Height Along Z<br>`eldr:fogZUp` | off | Height along Z for Z-up stages. | |
| Fog Film Jitter<br>`eldr:fogFilmJitter` | on | [film] Random froxel point per sample, no history: the film average is the medium. | 64 samples: RMSE 0.0014 to 0.00022 |
| Volume Step Size<br>`eldr:volumeStepSize` | float 1 | March step in voxels; larger is faster, coarser. | |
| Volume Max Steps<br>`eldr:volumeMaxSteps` | int 256 | Steps per pixel and volume box. | |
| Volume Shadow Steps<br>`eldr:volumeShadowSteps` | int 8 | Steps of the light-direction march. | volume.usda 21 ms at 8, about 17 at 4, about 11 at 1 |
| Volume Density Multiplier<br>`eldr:volumeDensity` | float 1 | Scales density (extinction). | |
| Volume Emission Multiplier<br>`eldr:volumeEmission` | float 1 | Scales emission. | |
| Volume Phase Anisotropy<br>`eldr:volumeAnisotropy` | float 0.2 | Phase g for volumes without a MaterialX volume material. | |
| Volume Resolution<br>`eldr:volumeResolution` | int 1 | 1 full, 2 half with depth-aware upsample. | volume.usda 22.0 to 7.3 ms |
| Volumes under Transparency<br>`eldr:volumeUnderTransparency` | on | Rays split at the nearest translucent surface: smoke behind glass is dimmed and refracted. Nearest layer only. | 4.2 vs 5.8 ms off |
| Volume Shadows<br>`eldr:volumeShadows` | on | Volumes darken surfaces and fog from the sun via the tint maps. | volume.usda sunShadows 1.0 vs 0.07 ms |
| Local Volume Shadows<br>`eldr:localVolumeShadows` | on | Same from local lights via the atlas tint map. Needs `eldr:volumeShadows` and `eldr:localShadows`. | Spot 0.17 ms, two sphere lights 0.8 ms; still frame 0 |
| Volume Mutual Shadows<br>`eldr:volumeMutualShadows` | on | Volumes shadow each other from the sun, local lights and the dome (from its pole, at half optical depth). | Three volumes 35.2 to 39.2 ms; lone volume free |
| Volume Default Fields<br>`eldr:volumeDefaultFields` | off | Volumes without a volume material also read Cd (tints scattering), temperature (blackbody glow) and emission. Off: density only, as Karma. | |

### Camera and film

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Temporal Anti-Aliasing<br>`eldr:taa` | on | Jitters the camera and resolves history. The film needs it too. Off: one aliased sample. | |
| TAA Samples<br>`eldr:taaSamples` | int 16 | Jitter cycle length; frames a still camera averages without the film; AOV averaging limit. | |
| Film Accumulation<br>`eldr:filmAccumulation` | on | Once still for 3 frames (or from frame 1 of a fresh history), plain Gaussian-filtered averaging replaces TAA. | 0.23 ms |
| Film Pixel Filter Radius<br>`eldr:filmFilterRadius` | float 1.5 | [film] Gaussian radius in pixels (0-2). | |
| Sample Dimensions<br>`eldr:sampleDimensions` | on | Each stochastic effect gets its own Sobol dimension and blue-noise rotation: uncorrelated, no screen patterns. Off: interleaved gradient noise. | None |
| Adaptive Sampling Noise Threshold<br>`eldr:adaptiveThreshold` | float 0 (off) | [film] A pixel whose standard error is under this after min samples stops. Try 0.002 for large flat areas. Not with motion blur time samples. In final quality `eldr:adaptiveFinalThreshold` applies when this is 0. | Off is bit-identical |
| Adaptive Sampling Converged Pixel Fraction<br>`eldr:adaptivePixelFraction` | float 0.999 | [film] Converged once this fraction of pixels stopped. 1 waits for all. | Cornell at 0.01: 27 samples at 0.9, 58 at 0.95 |
| Adaptive Sampling Min Samples<br>`eldr:adaptiveMinSamples` | int 16 | [film] Samples every pixel takes before it may stop. | |
| Object Motion Vectors<br>`eldr:objectMotion` | on | Object motion for TAA and blur. Off: camera only. | About 4 ms on crowd |
| Blend Shape Motion Vectors<br>`eldr:blendShapeMotion` | on | Blend shape weight changes count in skinned motion. | |
| Lens Jitter per Sample<br>`eldr:dofJitter` | on | [film] Each sample sees through a point of the aperture: true bokeh with occlusion. | None |
| Residual DOF Blur<br>`eldr:dofResidualBlur` | on | [film] With lens jitter, post DOF still runs per sample at 1/sqrt(film samples) of the blur, filling gaps at low sample counts. Off: pure lens jitter. | About 1.9 ms per sample at 1080p, with DOF |
| Residual Motion Blur<br>`eldr:motionBlurResidual` | on | [film] Each time sample spreads over its shutter stratum by a short gather along motion vectors: smooth blur, not discrete copies. | Gather per sample while something moves over 1 px per stratum |
| Sampled Motion<br>`eldr:sampledMotion` | on | Motion from the scene's transform samples across the shutter (instancers and camera too): a single frame blurs without a frame before. Off: change since last frame. Rigid motion only. | Two extra transform samplings per synced prim, open shutter only |
| Deformation Blur<br>`eldr:deformationBlur` | on | Meshes with moving points (point caches, simulations; not skinned) get per-point velocity from the shutter. Ray tracing sees points at the frame's time. | Moving meshes: 16 bytes per vertex |
| Lens Distortion<br>`eldr:lensDistortion` | on | Warps the finished frame (ids, depth too) with the camera's `lensDistortion:*` and OpenCV terms; scene renders wider (overscan). | 0.32 ms at 1080p; none without distortion |
| Post Processing<br>`eldr:postProcess` | on | Exposure, bloom, tonemap; TAA, film, SSR, post DOF, motion blur and upscaling depend on it. Off: raw linear HDR, one aliased sample. For linear EXRs keep on with `eldr:tonemapper` `none`, `eldr:autoExposure` off, `eldr:bloomIntensity` 0. | |

## Performance and debug

| Setting | Default | What it does | Cost |
|---|---|---|---|
| Depth Prepass<br>`eldr:depthPrepass` | on | Depth first: each pixel shades once; AO, SSR, SSGI, refraction, subsurface and ray tracing need it. Off only for A/B tests. | Off saves 14 ms on crowd but drops those effects |
| Frustum Culling (GPU)<br>`eldr:frustumCulling` | on | Culls instances outside the view. Off for A/B tests. | |
| Small Object Culling<br>`eldr:cullPixels` | float 0.5 | Skips instances under this screen radius in pixels; 0 = off. Use 0 for finals with tiny props. | |
| Share Frames with OpenGL<br>`eldr:shareFrame` | on | usdview and usdrecord keep colour and depth on the GPU. Falls back to readback if unsupported. Off for debugging. | 1080p 17.4 to 3.2 ms; 4K 67 to 9.6 ms |
| Overlap Readback while Moving<br>`eldr:overlapReadback` | on | Read back (Houdini): while the camera moves, a frame finishes on the GPU while the app draws and arrives with the next one, one frame behind. A still camera's frame is its own. Off: every frame waits for its own. | step4 1866x737, colour and depth, 11 ms app work: 20.3 to 14.0 ms a frame |
| Read Back in Place<br>`eldr:inPlaceReadback` | on | Read back (Houdini): colour, depth and ids stay where the GPU copied them, two buffers in turn, and the app maps them there instead of a copy of its own. Off: copied into the app's buffers. | macbeth 1866x737, 4 AOVs: 14.6 to 8.2 ms a frame |
| Pipeline Cache on Disk<br>`eldr:pipelineCache` | on | Saves compiled pipelines to `%LOCALAPPDATA%\Eldr\pipelines-*.bin` (`HDVK_PIPELINE_CACHE_DIR` moves it). | Crowd first frame 33.5 to 2.0 s; GI toggle 35 s to 79 ms; file about 16 MB |
| Prewarm Pipelines<br>`eldr:pipelinePrewarm` | on in Hydra, off for embedders | Compiles sets one toggle away (GI, SSGI, fog) on a worker thread so usdview doesn't freeze. Irrelevant for batch. | Half the cores in the background |
| Extra AOVs<br>`eldr:extraAovs` | on | Renders AOVs beyond colour, depth and ids when bound. | None unless bound. A bound AOV forces full size (no `eldr:renderScale`). |
| Render Scale<br>`eldr:renderScale` | float 1 | [interactive] Below 1 (min 0.25) renders smaller and TAA upscales. Film, final quality, DOF, motion blur, lens distortion and extra AOVs render full size. | Crowd moving: 27.0 ms at 1.0, 23.0 at 0.75, 16.4 at 0.5 |
| Interactive Noise Loop<br>`eldr:interactiveSamples` | int 8 | [interactive] Sample dimensions loop over this many samples (1-256) so TAA settles. | |
