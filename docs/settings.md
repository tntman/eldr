# Eldr render settings

Every setting is a Hydra render setting with the key `hdvk:<name>`. In usdview
open **Eldr > Render Settings** (grouped, with a filter; changed settings are
bold, one button resets them all). Scripts set them through
`UsdImagingGL.Engine.SetRendererSetting("hdvk:ssr", False)`. A setting that was
never set has the default shown here.

This page is checked by `tests/test_docs.py`: every key the renderer exposes
must appear below in backticks, in a table row. To add a setting, add a row in
the group `usdview/hdvkUsdview/settings.py` (`GROUPS`) puts it in.

How to read the tables:

- **Type, default**: flag = on/off, int, float, string. Ranges in the display
  name are clamped by the renderer.
- **[film]**: only acts under film accumulation or final quality (see
  [features.md](features.md#film-accumulation-interactive-taa-and-final-quality)).
  **[interactive]**: only while the camera or scene moves.
- **[RT]**: needs a GPU with ray tracing and is ignored without it
  (`HDVK_NO_RAY_TRACING=1` pretends there is none). **[prepass]**: needs
  *Depth Prepass*.
- **Cost**: GPU milliseconds measured on a GTX 1080 Ti at 1280x720, from the
  commit messages. "Crowd" is a 20,000-agent skinned crowd (about 21 ms with the
  defaults), "Kitchen" is Pixar's Kitchen_set. The GPU was often shared with
  other work, so treat differences under 1 ms as noise.
- Settings that cost nothing when off compile their shader code out
  (see [features.md](features.md#performance)); the first frame that uses a
  new combination compiles a pipeline set (seconds, unless cached).

Settings marked "off by default" are the expensive or special-case ones. For a
final render the usual recipe is: leave the defaults, turn on *Final Quality*,
raise *Film Samples*, and optionally turn on GI, SSGI, ray-traced shadows and
reflections.

## Performance and geometry

| Setting, key | Type, default | What it does, when to change it | Cost; related |
|---|---|---|---|
| Depth Prepass<br>`hdvk:depthPrepass` | flag, on | Draws depth first so every pixel is shaded once, and gives AO, SSR, SSGI, refraction, subsurface and ray-traced effects their depth. Turn off only to compare or for a scene where overdraw is trivial: those effects are then skipped. | Off saved 14 ms on the crowd (early measurement) but loses all of the dependent effects. |
| Frustum Culling (GPU)<br>`hdvk:frustumCulling` | flag, on | Culls instances outside the view on the GPU. Off draws every instance (small-object culling too). Only useful for A/B tests. | Related: *Small Object Culling*. |
| Small Object Culling<br>`hdvk:cullPixels` | float, 0.5 | Instances whose screen radius is under this many pixels are skipped; 0 disables. Raise for dense crowds in the distance; set 0 for final frames where tiny props must show. | Related: *Levels of Detail*. |
| Subdivision Surfaces<br>`hdvk:subdivision` | flag, on | Refines catmullClark, loop and bilinear meshes with OpenSubdiv to the refine level Hydra gives (usdview Complexity menu, `usdrecord --complexity`). Off draws the cage with smooth normals. | Memory and upload time grow about 4x per level (Kitchen_set at Very High: 35M triangles, 5.5 s to load). Related: *Subdivision Max Level*. |
| Subdivision Max Level<br>`hdvk:maxRefineLevel` | int, 3 | Clamps the refine level (0-8) the display style asks for. Lower it to keep heavy assets affordable. | Related: *Subdivision Surfaces*. |
| Levels of Detail<br>`hdvk:lod` | flag, on | Meshes of 1000 triangles or more get up to three coarser levels, picked per instance on the GPU so the error stays under about a pixel. Built on worker threads after upload; the render is not converged until they exist. Turn off for pixel-exact comparisons. | Crowd 68 ms to 28-34 ms; Kitchen at Very High 30 ms to 10.5 ms. Translucent and displaced meshes keep full detail. Related: *Level of Detail Bias*. |
| Level of Detail Bias<br>`hdvk:lodBias` | float, 0 | +1 allows twice the screen error (coarser), -1 half. Negative values for final quality: -1 costs the crowd 13.6M instead of 9.6M triangles. | Related: *Levels of Detail*. |
| Level of Detail Crossfade<br>`hdvk:lodFade` | flag, on | A level change fades over 8 frames: the instance draws in both levels and each pixel keeps one by a dither that moves every frame, which TAA or the film blends. Off: levels pop. Without TAA the dither shows as a stipple during those frames. | Static camera: nothing fades; the depth prepass of meshes with levels runs the alpha-tested pipeline (crowd +0.05 ms). Related: *Levels of Detail*. |
| Curves as Camera-Facing Ribbons<br>`hdvk:curveRibbons` | flag, off | Curves without normals draw as flat ribbons turned towards the eye (each pass's: shadows too) instead of 6-sided tubes; hair shading works on them. A curve's `primvars:eldr:ribbons` (bool) overrides it either way. Curves with normals stay oriented ribbons. | 2 triangles per step instead of 12: 100k grass blades 14.9 to 9.6 ms. Ray tracing sees them as lines. |
| Points as Camera-Facing Discs<br>`hdvk:pointSprites` | flag, off | Points draw as discs facing the eye (an octagon, shaded like a sphere) instead of icosahedra. A point prim's `primvars:eldr:sprites` (bool) overrides it either way. | 8 triangles per point instead of 20. Ray tracing sees them as points. |
| Displacement<br>`hdvk:displacement` | flag, on | Honours materials' displacement (UsdPreviewSurface `displacement`, MaterialX `displacement` shaders) in the vertex stage, so every pass sees the displaced surface. Needs enough vertices: use subdivision. | Displaced materials use their own pipelines; scenes without displacement pay nothing. Related: *Displacement Scale*, *Ray Tracing Sees Displacement*. |
| Displacement Scale<br>`hdvk:displacementScale` | float, 1 | Multiplies every material's displacement offset. | Changing it rebuilds displaced ray-tracing BLASes. |
| Displacement Graphs<br>`hdvk:displacementGraphs` | flag, on | Displacement whose input is a shading graph (MaterialX math, noise, several textures, primvars) runs that graph per vertex in every pass. Off: those materials don't displace (texture displacement is unaffected). | Only displaced pipelines run it. Graph-displaced surfaces shade with facet normals; ray tracing sees their rest surface. Related: *Displacement*. |
| Texture Anisotropic Filtering<br>`hdvk:anisotropy` | int, 16 | Anisotropic filter level for material textures (1 = off, up to 16). Lower to save bandwidth; for stills keep 16. | Small. |
| Share Frames with OpenGL<br>`hdvk:shareFrame` | flag, on | In usdview and `usdrecord` colour and depth stay on the GPU and are copied into OpenGL through external memory instead of a CPU readback. Falls back to readback by itself if the GPU or driver cannot share. Turn off to rule it out when debugging. | 1080p frame 17.4 ms to 3.2 ms, 4K 67 ms to 9.6 ms. |
| Extra AOVs<br>`hdvk:extraAovs` | flag, on | Renders AOVs beyond colour, depth and ids (normals, positions, albedo, light splits, Cryptomatte) when something binds them. | Nothing unless bound. A bound AOV forces full-size frames (no *Render Scale*). See [Output](features.md#output-aovs-exr-products-cryptomatte-light-splits). |
| Light Splits: Dome as Direct (Karma)<br>`hdvk:splitDomeDirect` | flag, on | Karma's convention for the light-split AOVs: the dome's unblocked light counts as direct (diffuse: its irradiance under AO or SSGI visibility; glossy: its reflection where no probe, mirror, SSR or traced hit replaced it). Off counts all dome light as indirect. The colour is the same either way. | Only in pipelines compiled with the light splits. See [Output](features.md#output-aovs-exr-products-cryptomatte-light-splits). |
| Write Render Products<br>`hdvk:renderProducts` | flag, on | Writes the active UsdRenderSettings' products as EXR files once the frame has converged (`usdrecord --renderSettingsPrimPath`, husk). Off renders but writes nothing. | One readback of the used AOVs per converged frame. |
| Linear EXR Beauty<br>`hdvk:productLinear` | flag, on | The beauty in a render product is the linear scene colour, exposed by the frame's (auto) exposure but with no bloom and no tonemapper, whatever *Tonemapper* says (centre of the test cube: 0.178 under aces, agx and none alike). Off writes the displayed, tonemapped beauty. The viewport is unchanged. | One more fullscreen pass, only while a product has a beauty var. Lens distortion is not applied to it. Related: *Write Render Products*, *Tonemapper*. |
| Pipeline Cache on Disk<br>`hdvk:pipelineCache` | flag, on | Saves compiled shading pipelines to `%LOCALAPPDATA%\Eldr\pipelines-*.bin` (override the folder with `HDVK_PIPELINE_CACHE_DIR`) so later runs skip compilation. Saving deletes the other files of the same driver (older shader builds), so one file per driver stays. Off: new sets are not saved. | Crowd first frame 33.5 s to 2.0 s on a second run; GI toggle 35 s to 79 ms. The file is about 16 MB. Related: *Prewarm Pipelines*. |
| Prewarm Pipelines<br>`hdvk:pipelinePrewarm` | flag, on in Hydra (off for embedders) | After a frame, compiles the shading sets one toggle away (GI, SSGI, fog, ...) on a worker thread so turning a feature on in usdview does not freeze. Irrelevant for batch renders, which never toggle. | Uses half the cores in the background. Crowd: GI toggle 35 s cold to 62 ms. Related: *Pipeline Cache on Disk*. |

## Lighting

| Setting, key | Type, default | What it does, when to change it | Cost; related |
|---|---|---|---|
| Dome Light Lighting (IBL)<br>`hdvk:imageBasedLighting` | flag, on | The dome light's diffuse (SH9) and specular (prefiltered cube) lighting. Off leaves only direct lights. Also gates the reflection probe. | Small. |
| Local Lights<br>`hdvk:localLights` | flag, on | Sphere, rect, disk and cylinder lights. Off for A/B. | Related: *Clustered Local Lights*. |
| Clustered Local Lights<br>`hdvk:clusteredLights` | flag, on | Culls local lights into a 16x9x24 cluster grid (63 lights per cluster at most). Off loops every light per pixel: only for comparing, or with very few lights. | Related: *Local Lights*. |
| Light Textures<br>`hdvk:lightTextures` | flag, on | Rect and disk lights multiply their radiance by `inputs:texture:file` (two taps per light: diffuse and specular). Off treats them as untextured. The texture is oriented as UsdLux specifies: st (0,0) at the light's +X +Y corner. | Crowd unchanged. |
| Emissive Surfaces as Lights<br>`hdvk:emissiveLights` | flag, on | Small bright emissive meshes (constant `emissiveColor` at or above the threshold) are also promoted to rect or sphere lights with shadows, up to 32. Large or textured emitters light the scene only through GI. | Related: *Emissive Light Threshold*, *Global Illumination*. |
| Emissive Light Threshold<br>`hdvk:emissiveLightThreshold` | float, 4 | Scene radiance of the brightest channel above which an emitter becomes a light. Lower to include dimmer panels. | |
| Dome Sun Extraction<br>`hdvk:domeSun` | flag, on | Texels of the HDRI brighter than the threshold are removed from the image-based light and become a shadowed distant light. It gives crisp shadows and cleaner lighting for HDRIs with a visible sun; scenes without a textured dome are unchanged. Turn off to light with the HDRI alone. | CPU work when the dome changes; none per frame. Related: *Dome Sun Threshold*. |
| Dome Sun Threshold<br>`hdvk:domeSunThreshold` | float, 10 | Luminance (scene radiance) above which a texel counts as sun. Lower if the sun is not found, raise if sky pieces are picked up. | |
| Ambient Occlusion (GTAO)<br>`hdvk:ambientOcclusion` | flag, on | Ground-truth AO on the dome's diffuse and specular light. [prepass] Replaced by SSGI when that is on. | About 0.34 ms. Related: *AO Radius*, *AO Directions*. |
| Ambient Occlusion Radius<br>`hdvk:aoRadius` | float, 0.5 | World-space radius of the AO horizon scan, in scene units. Match it to the scene's scale. | |
| Ambient Occlusion Directions<br>`hdvk:aoSlices` | int, 3 | Slices per pixel (1-8). More is smoother per sample. [film] averages many samples, so 3 is usually enough. | Linear in slices. |
| Global Illumination (RTX probes)<br>`hdvk:gi` | flag, off | DDGI: a grid of probes traces rays, so diffuse light is the bounced light of the scene instead of the dome's unoccluded light. [RT] Use for interiors and emissive lighting. | Kitchen 2-3 ms (1728 probes x 128 rays), crowd 0.2 ms, plus acceleration structure memory. Related: *GI Probes*, *GI Rays*, *GI Hysteresis*, *GI Preconverge*, *SSGI*. |
| GI Probes along the Scene<br>`hdvk:giProbes` | int, 16 | Probes along the longest side of the scene bounds (2-64, at most 16384 in total). More resolves smaller shadows and rooms. | Probe count times rays per frame. |
| GI Rays per Probe per Frame<br>`hdvk:giRaysPerProbe` | int, 128 | Rays each probe traces per frame (16-512). More converges faster and cleaner. | Linear. |
| GI Hysteresis<br>`hdvk:giHysteresis` | float, 0.97 | Weight of the probes' history (0-0.99). Higher is smoother but slower to react to lighting changes; for still renders it hardly matters. | Fresh probes need about 1/(1-h) frames to settle. |
| GI Preconverge<br>`hdvk:giPreconverge` | flag, on | [film] With GI, the film waits until fresh probes have settled before it starts averaging, so final frames do not keep the dark start-up light. | Cornell: 96 frames instead of 63; RMSE against a 1024-sample reference 0.0128 to 0.0047. |
| Screen-Space GI (replaces AO)<br>`hdvk:ssgi` | flag, off | Per-pixel indirect diffuse light gathered from the screen (GTAO's horizon scan, lighting from last frame's image), in place of AO. [prepass] No ray tracing needed. Adds close-range colour bleeding that probes blur away. | 0.67 ms at half resolution, 1.55 ms at full (Kitchen), against GTAO's 0.34. Related: *SSGI Steps*, *Radius*, *Resolution*, *Thickness*, *Bitmask*. |
| SSGI Steps<br>`hdvk:ssgiSteps` | int, 8 | Steps per direction and side (1-32). | |
| SSGI Radius<br>`hdvk:ssgiRadius` | float, 1 | World-space gather radius, scene units. Set to a fraction of the room size. | |
| SSGI Resolution Divisor<br>`hdvk:ssgiResolution` | int, 2 | 1 traces at full resolution, 2 at half (then denoised and upsampled depth-aware). | Full roughly doubles the cost. |
| Screen-Space Thickness Estimator<br>`hdvk:ssgiThickness` | flag, on | Makes SSGI and SSR rays respect thin features and grazing surfaces (pixel-wise refinement, thickness by footprint). Off steps evenly and treats objects as 5% of their distance thick. | Within noise on the crowd. |
| SSGI Visibility Bitmask<br>`hdvk:ssgiBitmask` | flag, off | EEVEE-style visibility bitmask (32 sectors per slice) instead of the highest horizon: light passes behind thin objects and thin objects do not darken a halo. Prefer for scenes with poles, railings, foliage. | Cornell 0.20 ms against 0.14 ms. Related: *SSGI Bitmask Object Thickness*. |
| Screen-Space Back-Face Hits<br>`hdvk:screenBackface` | flag, on | Blender 5.2's Backface option: an SSR or SSGI ray that meets the visible surface from behind (it went behind an object, judged by the surface's normal from depth) takes *Back-Face Hit Scale* of that surface's light, as a guess at the unseen side. Off: SSR takes such hits fully, the SSGI bitmask not at all and the SSGI horizon scan fully, as before. Less light leaks through objects into reflections and corners. | SSR hits: about 0.4 ms on the crowd (normal from depth at each hit); SSGI horizon 0.60 to 0.76 ms on the crowd, bitmask free. Related: *Back-Face Hit Scale*. |
| Screen-Space Back-Face Hit Scale<br>`hdvk:screenBackfaceScale` | float, 0.25 | The share of light a back-face hit takes (0-1): 0 treats objects as one-sided (no light from behind), 1 as double-sided. | Free. Needs *Back-Face Hits*. |
| SSGI Bitmask Object Thickness<br>`hdvk:ssgiBitmaskThickness` | float, 0.25 | How thick the bitmask takes every occluder, as a fraction of *SSGI Radius*: thin poles hide little, thicker settings hide more light behind them (the test pole's halo: 2.0/255 at 0.25, 37 at 4). | Free. Needs *SSGI Visibility Bitmask*. |
| Direct Light Clamp<br>`hdvk:directClamp` | float, 0 (off) | Limits the brightest channel of a pixel's direct light (sun and local) to this scene radiance, keeping hue. A firefly killer; costs energy. | Related: *Indirect Light Clamp*. |
| Indirect Light Clamp<br>`hdvk:indirectClamp` | float, 10 (0 = off) | The same for every bounce (SSGI input, SSR and ray-traced reflection hits, GI probe rays). The dome is not clamped. Ten leaves ordinary scenes unchanged (cornell with GI and SSGI: no visible difference) and caps only very bright bounces. | |

## Shadows

| Setting, key | Type, default | What it does, when to change it | Cost; related |
|---|---|---|---|
| Sun Shadows (cascades)<br>`hdvk:shadows` | flag, on | Cascaded shadow maps for the first shadow-casting distant light. | Related: *Resolution*, *Cascades*. |
| Sun Shadow Resolution<br>`hdvk:shadowResolution` | int, 2048 | Texels per cascade side (256-8192). Raise for large scenes with fine contact detail. | Memory and time grow with the square. |
| Sun Shadow Cascades<br>`hdvk:shadowCascades` | int, 4 | 1-4. One cascade saves about 12 ms on the crowd (early measurement) but spreads the resolution thin. | |
| Local Light Shadows<br>`hdvk:localShadows` | flag, on | Shadows for sphere, rect, disk and spot lights from a 4096x4096 atlas (spots with a cone up to 80 degrees get one tile, others six cube faces). Lights nearest the camera get tiles first; the rest stay unshadowed. | Atlas is cached while the scene is still. |
| Local Light Shadow Resolution<br>`hdvk:localShadowResolution` | int, 512 | Texels per tile (64-2048). Lower resolution means more lights get tiles. | |
| Soft Shadows (PCSS)<br>`hdvk:softShadows` | flag, on | Contact-hardening penumbrae for local lights from their size. Off uses 3x3 PCF. | Related: *Soft Shadow Samples*, *Jittered Light Shape*. |
| Soft Sun Shadows<br>`hdvk:softSunShadows` | flag, on | The same for the sun from the distant light's `inputs:angle`. A sun without an angle keeps 3x3 PCF. | Crowd 1.27 ms (was 2.74 ms before the blocker search was bounded). |
| Soft Shadow Samples<br>`hdvk:softShadowSamples` | int, 16 | Blocker search and filter taps (1-16). | Linear. |
| Translucent Shadows<br>`hdvk:translucentShadows` | flag, on | Glass, water and tinted translucent meshes tint and lighten shadows instead of casting none. Volumes cast into the same maps. | 0.04 ms (glass test scene); the maps are 32 MB each at the sun's default size. Related: *Divisor*. |
| Translucent Shadow Resolution<br>`hdvk:translucentShadowDivisor` | int, 2 | Tint map size as 1/N of the shadow maps (1, 2 or 4). Also sets the volume shadow resolution. | |
| Ray-Traced Shadows<br>`hdvk:rtShadows` | flag, off | [RT][prepass] Traced shadows for the sun and the three most important local lights instead of shadow maps. Soft from the sun's angle and light size, noisy per frame; the film averages it. Best for final renders. | About 10 ms in Kitchen. Related: *Rays per Light*, *Skinned Instances*, *Ray Tracing Sees Displacement*. |
| Ray-Traced Shadow Rays per Light<br>`hdvk:rtShadowSamples` | int, 4 | Rays per light and pixel (1-16). With the film, 1 is enough: samples add up. | Linear. |
| Ray-Traced Skinned Instances<br>`hdvk:rtSkinnedInstances` | int, 16 | [RT] How many skinned instances (the largest on screen) block rays, for every ray-traced effect (0-256). The rest do not cast traced shadows or reflect. | Kitchen-size scenes: 0.45 ms for 16, 0.69 ms for 90 walking characters; crowd 0.68 ms for 16, 2.7 ms for 256. |
| Ray Tracing Sees Displacement<br>`hdvk:rtDisplacement` | flag, on | [RT] Displaced meshes' BLASes are built from the displaced vertices, so traced shadows, reflections and GI hit what the raster draws. | Steady frames cost nothing; rebuilds when the scale changes. |
| Jittered Light Shape per Sample<br>`hdvk:shadowJitter` | flag, on | [film, final quality only] In a film run that started in final mode (a fresh history as usdrecord's first frame, or *Final Quality*), each sample renders the shadow maps from a point of the light (sun disc, rect or disk area, cylinder axis, inside a sphere) with hard lookups, so the average is the true penumbra rather than PCSS's approximation. Needs *Soft Shadows* (and *Soft Sun Shadows* for the sun). Interactive films, such as usdview's still camera after a change, keep PCSS. | Sun, 128 samples: worst error 0.016 against PCSS 0.065. No measurable frame-time change. |

## Reflections

| Setting, key | Type, default | What it does, when to change it | Cost; related |
|---|---|---|---|
| Screen Space Reflections<br>`hdvk:ssr` | flag, on | Marches reflection rays against the depth buffer and shades hits from last frame's image. [prepass] Needs post processing. | 6 ms on the crowd (early measurement). Related: *Reflection Ray Steps*, *Thickness Estimator*. |
| Reflection Ray Steps<br>`hdvk:ssrSteps` | int, 32 | Steps per ray (1-256). Raise if long reflections break up. | Linear. |
| Denoised Glossy SSR<br>`hdvk:ssrDenoise` | flag, off | One GGX-sampled ray per pixel with a ratio-estimator spatial filter, temporal reprojection and a bilateral filter, for glossy (not only mirror) reflections up to roughness 0.8. Off: one mirror ray, faded out between roughness 0.3 and 0.6. Not with ray-traced reflections. | Denoiser passes 1.1 ms; tracing rougher pixels raises the crowd's shading from 12.5 ms to 21.4 ms. |
| Ray-Traced Reflections<br>`hdvk:rtReflections` | flag, off | [RT][prepass] Traced reflections replace the dome's and SSR's up to roughness 0.7, shaded at hits (direct light, dome, emission, probes' GI). Sees off-screen geometry. | About 15 ms in Kitchen. Related: *Rays per Pixel*, *Skinned Instances*, *Indirect Light Clamp*. |
| Ray-Traced Reflection Rays per Pixel<br>`hdvk:rtReflectionSamples` | int, 1 | Rays per pixel (1-16). The film makes 1 sufficient. | Linear. |
| Reflection Probe<br>`hdvk:reflectionProbe` | flag, off | Captures one cube of the scene from its centre (or *Position*) and uses it, parallax corrected, instead of the dome's specular. Recaptured when lights, materials, dome or (once settled) geometry change; the render is not converged until then. Follows *Dome Light Lighting*. | Capture: Kitchen 6.5 ms, crowd 122 ms, once; lookup about 1 ms per frame. Related: *Authored Probes*, *Normalize*. |
| Reflection Probe Resolution<br>`hdvk:probeResolution` | int, 128 | Cube face size (16-512, power of two). | Prefiltered on the CPU (about 10 ms at 128). |
| Reflection Probe Position<br>`hdvk:probePosition` | string, empty | "x y z" capture point of the automatic probe; empty is the scene bounds' centre. | |
| Authored Reflection Probes<br>`hdvk:authoredProbes` | flag, on | Meshes flagged `primvars:hdvk:reflectionProbe` are the probes (up to 8, smallest boxes first), each a box of influence and parallax. Needs *Reflection Probe* on. See [authoring](features.md#usd-authoring-conventions). | |
| Normalize Reflection Probes<br>`hdvk:probeNormalize` | flag, on | Scales a probe's reflection by local ambient light (GI or SSGI) over the ambient light it saw at capture, so dark corners do not reflect a lit room. Only has an effect with GI or SSGI on; clamped to 4x. | |
| Planar Reflections<br>`hdvk:planarReflections` | flag, off | Flat glossy mirrors (flagged `primvars:hdvk:planarReflection`, or else the largest flat, glossy, opaque mesh) reflect the scene rendered mirrored, at a fraction of the frame size. Use for mirrors and polished floors that SSR cannot cover. | About 0.1 ms in the crowd scene; each mirror in view costs one extra scene render. Related: *Scale*, *Mirrors*. |
| Planar Reflection Scale<br>`hdvk:planarScale` | float, 0.5 | Size of the mirrored render, as a fraction of the frame (0.1-1). | |
| Planar Reflection Mirrors<br>`hdvk:planarCount` | int, 4 | Mirrors rendered per frame, the largest first (at most 4). | One render each. |

## Materials

| Setting, key | Type, default | What it does, when to change it | Cost; related |
|---|---|---|---|
| Normal Maps<br>`hdvk:normalMaps` | flag, on | Applies normal maps. | |
| Specular Workflow<br>`hdvk:specularWorkflow` | flag, on | UsdPreviewSurface with `useSpecularWorkflow = 1` reflects with F0 = `specularColor`. Off uses the metallic workflow's 0.04. | |
| Clearcoat<br>`hdvk:clearcoat` | flag, on | A second GGX lobe: UsdPreviewSurface `clearcoat`, standard_surface `coat`, OpenPBR `coat_*`. Energy-conserving. | Materials with lobes use their own pipelines; plain materials do not pay. |
| Sheen<br>`hdvk:sheen` | flag, on | Microfiber (Charlie) lobe: standard_surface `sheen`, OpenPBR `fuzz_*`. | Same. |
| Thin Film Iridescence<br>`hdvk:thinFilm` | flag, on | Thin-film interference on the specular: standard_surface `thin_film_thickness`, OpenPBR `thin_film_*`. | Same. |
| Anisotropic Specular<br>`hdvk:anisotropicSpecular` | flag, on | Stretches the highlight along the surface's u direction (standard_surface `specular_anisotropy`/`specular_rotation`, OpenPBR `specular_roughness_anisotropy`). Meshes without uvs brush around world up. | Same. |
| Hair Shading<br>`hdvk:hairShading` | flag, on | BasisCurves shade as hair: Kajiya-Kay two-lobe highlights, no SSR. Opt a prim out with `primvars:eldr:hair = 0`. | Crowd unchanged. |
| Toon Shading<br>`hdvk:toon` | flag, on | Materials with `inputs:eldr:toonBands` quantise their lighting into flat bands. | |
| Toon Bands for Every Material<br>`hdvk:toonBands` | int, 0 (off) | Bands per unit of light applied to all other materials too. Turn on for a whole-scene NPR look. | Banding every pixel had cost 0.3-0.4 ms before it became a compiled-out feature. |
| Textured Lobe Inputs<br>`hdvk:texturedLobes` | flag, on | Coat, sheen, anisotropy, thin film, specularColor and IOR inputs may be textures or graphs, evaluated per pixel by the material program. Off reads their constants. | Only materials with such connections. |
| MaterialX Graphs<br>`hdvk:materialGraphs` | flag, on | Compiles MaterialX/UsdShade graphs behind material inputs into per-pixel programs. Off: plain inputs and textures only. | A full-screen graph adds about 2.5 ms at 1080p; scenes without graphs are unchanged. Related: *Procedural Textures*. |
| Procedural Textures<br>`hdvk:proceduralTextures` | flag, on | MaterialX noise nodes (noise, fractal, cellnoise, worleynoise, position, normal) in programs. Off: noise reads 0. | Full-screen fractal3d (4 octaves) 0.74 ms; worleynoise3d 0.5 ms. |
| Subsurface Scattering<br>`hdvk:subsurface` | flag, on | standard_surface/OpenPBR subsurface: a separable screen-space Gaussian blur per colour channel of the diffuse light, plus the sun shining through thin parts. [prepass] | About 0.3 ms, only in scenes with subsurface materials. Related: *Subsurface Samples*. |
| Subsurface Samples<br>`hdvk:subsurfaceSamples` | int, 15 | Taps per direction (1-64). | |

## Transparency

| Setting, key | Type, default | What it does, when to change it | Cost; related |
|---|---|---|---|
| Textured Transmission and Subsurface Inputs<br>`hdvk:texturedTransmission` | flag, on | Transmission, transmission colour, subsurface weight, colour and radius may be textures or graphs. Off reads constants. | |
| Translucent Objects<br>`hdvk:translucency` | flag, on | Draws materials with opacity below 1. Off: they are not drawn. | Scenes without them skip the pass. |
| Order-Independent Transparency<br>`hdvk:oit` | flag, on | Weighted blended OIT (McGuire and Bavoil) for translucent meshes. Off sorts meshes back to front, which can misorder one mesh's own triangles. | 18 bytes per pixel and one full-screen pass. Related: *OIT Depth Weight*. |
| OIT Depth Weight<br>`hdvk:oitDepthWeight` | float, 4 | Exponent of the distance weight (0 = plain average). Raise if far layers show through near ones. | |
| Refraction<br>`hdvk:refraction` | flag, on | Materials with transmission or an `ior` bend and tint a copy of the opaque colour behind them; rough ones are blurred. Screen-space: what is off screen is not refracted. [prepass] | About 0.2 ms in the glass test scene. |
| Refraction Blur Samples<br>`hdvk:refractionSamples` | int, 8 | Taps for rough refraction (1-16). | |
| Refraction Back Faces<br>`hdvk:refractionBackFaces` | flag, on | Back faces of refracting meshes are drawn too (water from below with Snell's window and total internal reflection; the far side of a glass ball). Off discards back faces. | |
| Refraction Through Layers<br>`hdvk:multiRefraction` | flag, on | Glass behind glass: the background is copied again after each refracting layer. | About 0.1 ms per copy at 1280x720. Related: *Refraction Layers*. |
| Refraction Layers<br>`hdvk:refractionLayers` | int, 4 | Most background copies per frame (1-16); further layers share the last. | |
| Holdouts<br>`hdvk:holdouts` | flag, on | Matte objects draw black with zero alpha but keep depth, ids and shadows. Off draws them normally. | None. |
| Translucent Holdouts Cut Alpha<br>`hdvk:holdoutAlpha` | flag, on | A translucent holdout cuts colour and alpha by its opacity instead of drawing nothing. | A small pass only when such holdouts exist. |

## Fog and volumes

Fog is height fog in a froxel grid driven only by these settings (USD has no
fog schema). Volumes are OpenVDB `UsdVolVolume` prims (builds against a USD
with OpenVDB only).

| Setting, key | Type, default | What it does, when to change it | Cost; related |
|---|---|---|---|
| Volumetric Fog<br>`hdvk:fog` | flag, off | Height fog lit by every light (sun cascades, local lights, dome), over surfaces and sky, with god rays. | 0.19 ms on the crowd, shading about 0.5 ms more. |
| Fog Density<br>`hdvk:fogDensity` | float, 0.02 | Extinction per scene unit at the base height. Scale to the scene's size. | |
| Fog Height<br>`hdvk:fogHeight` | float, 0 | Base height (Y, or Z with *Along Z*). | |
| Fog Height Falloff<br>`hdvk:fogHeightFalloff` | float, 0 | Density is multiplied by exp(-falloff x (height - base)); 0 is uniform. | |
| Fog Anisotropy<br>`hdvk:fogAnisotropy` | float, 0.6 | Henyey-Greenstein g (-1..1); towards 1 gives shafts towards lights. | |
| Fog Ambient from GI Probes<br>`hdvk:fogGiAmbient` | flag, on | With GI on, the dome's light in the fog comes from the probes, so closed rooms are not filled with outdoor haze. Without GI nothing changes. | Fog pass 0.2 ms to 0.47 ms with GI. |
| Fog Color<br>`hdvk:fogColor` | string, "1 1 1" | Scattering albedo, "r g b". | |
| Fog Max Distance<br>`hdvk:fogMaxDistance` | float, 0 | View depth the froxels reach; 0 is the scene's far end. Shorten for more depth resolution near the camera. | |
| Fog Grid Size<br>`hdvk:fogGridSize` | int, 160 | Froxels across (16-512), 64 depth slices. | Grid 64: 0.06 ms; 256: 0.41 ms. |
| Fog Height Along Z<br>`hdvk:fogZUp` | flag, off | Measures height along Z for Z-up stages. | |
| Fog Film Jitter<br>`hdvk:fogFilmJitter` | flag, on | [film] Every froxel is sampled at a random point per film sample with no history, so the film's average is the medium itself. | 64 samples: RMSE 0.0014 to 0.00022 against 1024 samples. |
| Volumes (OpenVDB)<br>`hdvk:volumes` | flag, on | Ray-marches OpenVDB volumes (NanoVDB on the GPU) over the shaded image. Only exists in builds against a USD with OpenVDB. | See below. |
| Volume Step Size<br>`hdvk:volumeStepSize` | float, 1 | March step in voxels. Larger is faster, coarser. | |
| Volume Max Steps<br>`hdvk:volumeMaxSteps` | int, 256 | Cap on steps per pixel and volume box. | |
| Volume Shadow Steps<br>`hdvk:volumeShadowSteps` | int, 8 | Steps of the light-direction shadow march. | volume.usda: 21 ms at 8, about 17 at 4, about 11 at 1 (early). |
| Volume Density Multiplier<br>`hdvk:volumeDensity` | float, 1 | Scales the material's density (extinction). | |
| Volume Emission Multiplier<br>`hdvk:volumeEmission` | float, 1 | Scales emission (temperature blackbody and emission fields). | |
| Volume Phase Anisotropy<br>`hdvk:volumeAnisotropy` | float, 0.2 | Phase g for volumes without a MaterialX volume material. | |
| Volume Resolution<br>`hdvk:volumeResolution` | int, 1 | 1 full resolution, 2 half with a depth-aware upsample. | volume.usda: 22.0 ms to 7.3 ms. |
| Volumes under Transparency<br>`hdvk:volumeUnderTransparency` | flag, on | Rays are split at the nearest translucent surface, so smoke behind glass is dimmed and refracted. Only the nearest layer splits. | volume pass 4.2 ms against 5.8 ms off (glass+volume scene). |
| Volume Shadows<br>`hdvk:volumeShadows` | flag, on | Volumes darken surfaces from the sun through the tint maps. One depth per texel, no shadows from local lights. | sunShadows pass 1.0 ms against 0.07 ms on volume.usda. |

## Camera and image

| Setting, key | Type, default | What it does, when to change it | Cost; related |
|---|---|---|---|
| Temporal Anti-Aliasing<br>`hdvk:taa` | flag, on | Jitters the camera and resolves history (Catmull-Rom, YCoCg clipping). The film also needs TAA on. Off gives one aliased, unjittered sample. | Related: everything under film. |
| TAA Samples<br>`hdvk:taaSamples` | int, 16 | Length of the pixel jitter cycle; also how many frames a still camera averages without the film, and the AOV averaging limit. | |
| Render Scale<br>`hdvk:renderScale` | float, 1 | [interactive] Below 1 (down to 0.25) the scene renders at that fraction while TAA resolves, and TAA upscales into a full-size history. Film, final quality and frames with DOF, motion blur, lens distortion or extra AOVs always render full size. | Crowd moving camera: 27.0 ms at 1.0, 23.0 at 0.75, 16.4 at 0.5. |
| Film Accumulation<br>`hdvk:filmAccumulation` | flag, on | Once the camera and scene are still for 3 frames (and from the first frame of a fresh history such as `usdrecord`), samples are averaged plainly with a Gaussian pixel filter instead of TAA, avoiding TAA's clipping bias. | 0.23 ms per frame. Related: *Film Samples*, *Final Quality*. |
| Film Samples<br>`hdvk:filmSamples` | int, 64 | [film] Samples until the render reports converged. Raise to 256 or more for noise-free finals. | Time is linear in samples. |
| Film Pixel Filter Radius<br>`hdvk:filmFilterRadius` | float, 1.5 | [film] Gaussian reconstruction radius in pixels (0-2). Lower is sharper, higher softer. | |
| Final Quality<br>`hdvk:finalQuality` | flag, off | [film] Offline mode: the film starts from the first sample after any change, from scratch, never from the TAA image. Also on when Hydra's `enableInteractive` is false (husk, hdPrman convention). | Related: all film settings. |
| Sample Dimensions<br>`hdvk:sampleDimensions` | flag, on | Each stochastic effect (AO, SSGI, SSR, shadows, refraction, volumes, fog) gets its own Sobol dimension and blue-noise rotation per sample, so effects are uncorrelated and the film converges without screen patterns. Off uses interleaved gradient noise. | None. |
| Interactive Noise Loop<br>`hdvk:interactiveSamples` | int, 8 | [interactive] While TAA resolves, the sample dimensions loop over this many samples (1-256) so TAA's history settles on a short pattern. | |
| Adaptive Sampling Noise Threshold<br>`hdvk:adaptiveThreshold` | float, 0 (off) | [film] A pixel whose mean's standard error (compressed luma) is under this after *Min Samples* takes no more samples; the film converges when none is left. Try 0.002 for scenes with large flat areas. Applies to every film; in final quality *Final Quality Threshold* is used when this is 0. | On the test scenes nothing stopped early, because edge anti-aliasing is variance too. Off is bit-identical. |
| Adaptive Sampling Threshold, Final Quality<br>`hdvk:adaptiveFinalThreshold` | float, 0.01 (0 = off) | [film] The adaptive threshold used in final quality (a film run that started fresh or with *Final Quality*) when *Noise Threshold* is 0; interactive films stay non-adaptive. Samples to converge (640x360, 64 film samples): sunangle 63 / 27 / 17 at 0.005 / 0.01 / 0.02, cornell 64 / 64 / 64 and 37 at 0.03, Kitchen 64 at every value up to 0.1. At 0.01 the image differs from the full film by 0.02 (sunangle) and 0.13 (cornell) of 255 on average. | Fewer samples. A bright feature under 1% of the pixels can stop early. Related: *Converged Pixel Fraction*. |
| Adaptive Sampling Converged Pixel Fraction<br>`hdvk:adaptivePixelFraction` | float, 0.999 | [film] An adaptive film counts as converged once this fraction of the pixels has stopped sampling, so a few noisy edge pixels don't run it to *Film Samples*. The default is high so small bright features (a defocused highlight under 1% of the frame) still get their samples. 1 waits for every pixel. | Cornell at 0.01: 27 samples at 0.9, 58 at 0.95. |

| Adaptive Sampling Min Samples<br>`hdvk:adaptiveMinSamples` | int, 16 | [film] Samples every pixel takes before it may stop. The maximum is *Film Samples*. | |
| Object Motion Vectors<br>`hdvk:objectMotion` | flag, on | Moving objects' motion for TAA and motion blur. Off: camera motion only. | 4 ms on the crowd (early). |
| Blend Shape Motion Vectors<br>`hdvk:blendShapeMotion` | flag, on | Blend shape weight changes are included in skinned motion vectors. | |
| Depth of Field<br>`hdvk:depthOfField` | flag, on | Thin-lens blur from the scene camera's `fStop`, `focalLength` and `focusDistance` (perspective cameras only; usdview's free camera has no lens). | Post DOF about 1.9 ms at 1080p. Related: *Lens Jitter*. |
| Lens Jitter per Sample<br>`hdvk:dofJitter` | flag, on | [film] Each film sample sees through a point of the aperture (sheared so the focus plane stays), so the average is true bokeh with correct occlusion; the post DOF is skipped then. | No measurable cost. |
| Motion Blur<br>`hdvk:motionBlur` | flag, on | Blur along motion for an open camera shutter (`shutter:open`, `shutter:close`). Interactive frames use a post blur (McGuire). | Post blur about 0.2 ms. Related: *Time Samples*. |
| Sampled Motion<br>`hdvk:sampledMotion` | flag, on | Motion for the blur and time samples comes from the scene's own transform samples through the camera's shutter (Hydra's `SampleTransform`, instancers too, and the camera's own), so a single frame (usdrecord, husk, the first frame of a sequence) blurs with no frame before it. Off: motion is the change since the last rendered frame. Rigid motion only: deforming points and skinning still need the frame before. | Two extra transform samplings per prim whose transform syncs (every prim on the first frame), only with an open shutter. Related: *Motion Blur*, *Time Samples*. |
| Motion Blur Time Samples<br>`hdvk:motionBlurTimeSamples` | flag, on | [film] Each film sample renders the whole scene at a time in the shutter interval (shadows, rays and refraction included), linear from the previous frame's transforms, joints, blend shapes and camera. Off keeps the post blur. | Crowd: 1.7-2.1 ms per sample for the shutter pass. |
| Lens Distortion<br>`hdvk:lensDistortion` | flag, on | Warps the finished frame (ids and depth too) with the camera's `lensDistortion:*` attributes and OpenCV terms; the scene renders wider (overscan) for the warp. | 0.32 ms at 1080p, nothing without a distorting camera. |
| Post Processing<br>`hdvk:postProcess` | flag, on | Exposure, bloom and tonemap, and everything that rides on that chain: TAA, film, SSR, post DOF, motion blur and upscaling all need it. Off returns the raw linear HDR scene colour, premultiplied by coverage, with a single aliased sample. For linear EXRs to grade in comp keep it on and set *Tonemapper* `none`, *Auto Exposure* off and *Bloom Intensity* 0 instead. | Related: *Tonemapper*. |
| Auto Exposure<br>`hdvk:autoExposure` | flag, on | Histogram-based exposure that adapts over time. Turn off for finals so brightness is deterministic, and set *Exposure Compensation*. | 0.11 ms. |
| Exposure Compensation<br>`hdvk:exposureCompensation` | float, 0 | EV added; also the fixed exposure when auto is off. | |
| Bloom Intensity<br>`hdvk:bloomIntensity` | float, 0.04 | Strength of the bloom (0 disables). | |
| Tonemapper<br>`hdvk:tonemapper` | string, aces | `aces`, `agx`, `agxPunchy` or `none` (exposed linear, unclamped). Use `none` when the EXR will be graded in comp. | |
