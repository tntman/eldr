# Eldr showcase scenes

One folder per feature, named `YYYY-MM-DD_NN_feature`. Each has a `scene.usda`, a `README.md` (what the feature is, which nodes to compare, what to look at) and a generated `scene.hip`.

Every scene has the same three render settings prims and nodes: `eldr_default` (Eldr, nothing set), `eldr_final` (the final-render settings ticked) and `karma` (Karma to match), plus the feature's own A/B prims. Open `showcase/<folder>/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then the A/B nodes.

Without Houdini: `usdenv usdview --renderer Eldr showcase\<folder>\scene.usda` and pick a settings prim, or `husk -R Eldr -s /Render/<name> showcase/<folder>/scene.usda`. `houdini/make_showcase_hip.py` (hython) rebuilds the `.hip` files.

## Lights

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_03_emissive_lights` | Glowing meshes light the scene as lights do, with shadows (`eldr:emissiveLights`). | `emissiveLightsOn` vs `emissiveLightsOff` |
| `2026-10-29_15_light_linking` | A light can skip objects (light linking) and its shadows can skip casters (shadow linking), from UsdLux collections as Houdini's Light Linker writes them. | `default` vs `noLinking` (`eldr:lightLinking` off), `traced` (the same links with traced shadows) |
| `2026-10-29_17_colour_limit` | Karma's colour limit clamps what the camera sees of emitters and the light they bounce, with a full-HDR option for comp (`eldr:colorLimit`, `eldr:indirectClamp`). | `limit20` (Karma's default) vs `fullHdr` vs `off`, `karmaName5` (Karma's `karma:global:colorlimit` 5) |
| `2026-10-29_22_light_textures` | A rect or disk light's `inputs:texture:file` colours its light, in direct light, bounce light and reflections (`eldr:lightTextures`). | `lightTextures` vs `lightTexturesOff` |
| `2026-10-29_24_ies_profiles` | A light's IES profile (`inputs:shaping:ies:file`) shapes its light, as in Karma (`eldr:lightIes`). | `ies` vs `noIes` |
| `2026-10-30_06_many_lights` | Hundreds of small lights shaded through clusters, each pixel lit only by the lights that reach it (`eldr:clusteredLights`). | `lights` vs `clustersOff`, `localLightsOff` |
| `2026-10-30_07_light_shapes` | Sun, sphere, rect, disk and tube lights give highlights of their own shape, and each light with Render Light Geometry on shows its shape (`eldr:lightShapesVisible` is the master switch). | `shapes` vs `shapesVisible` |
| `2026-10-30_08_dome_sun` | The HDRI's sun is taken out of the dome and cast as a shadowed distant light (`eldr:domeSun`). | `domeSun` vs `domeSunOff` |
| `2026-10-30_26_further_sun_shadows` | A second distant light, such as the HDRI's extracted sun beside the scene's own sun, casts shadows from a map of its own (`eldr:extraSunShadowMaps`), or traced ones while ray tracing runs (`eldr:rtExtraSunShadows`). | `furtherSuns` vs `furtherSunsOff` vs `furtherSunsTraced` |
| `2026-10-30_09_ambient_occlusion` | The dome's light is darkened in corners and gaps by screen-space ambient occlusion (`eldr:ambientOcclusion`). | `ambient` vs `aoOff`, `aoWide` (`eldr:aoRadius` 2) |
| `2026-10-30_10_shadow_modes` | Shadows from maps (sun cascades, light atlas) softened by PCSS and jitter, hard PCF, or traced (`eldr:shadowJitter`, `eldr:softShadows`, `eldr:rtShadows`). | `maps` (default) vs `pcss` vs `hard` vs `traced` |
| `2026-10-30_16_light_path_intensity` | Direct and indirect light scaled separately in the final pixel (`eldr:directIntensity`, `eldr:indirectIntensity`). | `neutral` vs `indirectBoost` vs `directDim` |

## Materials

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_10_curve_styles` | Each curve or points prim can set its width scale, tubes or ribbons, and sphere points, with Eldr's names winning over Karma's. | `styles` (the per-object values) vs `ribbons` (`eldr:curveRibbons`: every curve without its own style becomes a ribbon); `eldr_default` vs `karma` (Karma reads only the karma names) |
| `2026-10-29_01_opacity_mode` | UsdPreviewSurface's `opacityMode` "presence" fades the whole surface, highlight included, while "transparent" keeps the highlight (`eldr:opacityMode`). | `presence` vs `opacityModeOff` (both quads shaded as transparent) |
| `2026-10-29_09_transparency_order` | Glass, translucent meshes and smoke are composited in one depth order, so what is behind a pane is seen through it. | `default`, `sorted` (`eldr:oit` off), `oneLayer` (`eldr:refractionLayers` 1), `volumesOverGlass` (`eldr:volumeUnderTransparency` off), `final` (traced refraction) |
| `2026-10-29_16_materialx_nodes` | MaterialX compositing nodes, glTF PBR, Disney and unlit surfaces, and Karma's Ramp Const, Hex Tiled Texture and Voronoi nodes. | `graphsOn` vs `graphsOff` (`eldr:materialGraphs` off), `proceduralsOff` (`eldr:proceduralTextures` off) |
| `2026-10-29_21_displacement_bound` | A material's displacement shader can state its largest offset (`inputs:eldr:displacementBound`) so culling keeps a surface that a displacement graph moves far. | `culled` (frustum culling on, the default) vs `noCulling` (`eldr:frustumCulling` off) |
| `2026-10-30_13_subsurface` | Light scattering under the surface of skin, jade and leaves (`eldr:subsurface`). | `subsurface` vs `subsurfaceOff`, `subsurfaceQuality` |
| `2026-10-30_18_displacement` | UsdPreviewSurface and MaterialX displacement move the surface, with shadows (`eldr:displacement`). | `displacement` vs `displacementOff`, `displacementHalf` |
| `2026-10-30_19_curves_points` | Every curve basis as tubes or ribbons, points as spheres or discs, and hair shading. | `curves` vs `ribbons`, `sprites`, `hairOff` |
| `2026-10-30_21_textures_colour` | Texture colour spaces (ACEScg, Rec.709), UDIM tiles and anisotropic filtering (`eldr:anisotropy`). | `textures` vs `aniso1` |
| `2026-10-30_22_material_lobes` | Clearcoat, sheen, fuzz, thin film, anisotropic highlights and hair, plus toon shading. | `lobes` vs `lobesOff`, `toon` (`eldr:toonBands` 3) |

## Ray tracing

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_04_gi_final_gather` | Bounced light from GI probes, a traced final gather or screen-space GI. | `probesOnly`, `finalGather`, `ssgi`, `giPlusSsgi`; tuning: `finalGatherNoRelocation`, `finalGatherDistance0`/`05`, `denoise0`/`1`, `rays1`/`4` |
| `2026-10-28_09_traced_reflections` | Reflections traced against the whole scene, including what is behind the camera or off screen (`eldr:rtReflections`). | `traced` vs `screenSpace` (`eldr:rtReflections` off), `tracedNoHitShadows` (`eldr:rtHitShadows` off) |
| `2026-10-29_03_planar_reflection_blur` | A rough planar mirror blurs its reflection by its roughness (`eldr:planarBlur`). | `blurred` vs `sharp` |
| `2026-10-29_13_caustics` | Light focused through glass onto other surfaces, by progressive photon mapping (`eldr:caustics`). | `caustics` vs `noCaustics`, `caustics8`, `causticsFewPhotons` |
| `2026-10-30_11_screen_space_reflections` | Reflections ray-marched in the last frame's image, fading out with roughness (`eldr:ssr`). | `ssr` vs `ssrOff` |
| `2026-10-30_23_reflection_probe` | A cube captured around the scene supplies reflections where the screen has none (`eldr:reflectionProbe`). | `probe` vs `probeOff` |

## Volumes

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-29_05_coloured_volume_shadows` | Coloured smoke casts coloured shadows, per channel, with shadow maps or traced shadows. | `maps` vs `shadowsOff` (`eldr:volumeShadows`), `traced` (`eldr:rtShadows`), `halfResolution` (`eldr:volumeResolution` 2) |
| `2026-10-29_08_volume_default_material` | A volume with no material scatters as Karma's default medium: white, no absorption, isotropic, the dome lighting it from its upper half. | `newPhase` (the default) vs `oldPhase` (`eldr:volumeAnisotropy` 0.2) |
| `2026-10-29_18_volume_motion_blur` | Volumes blur along their `vel` field (a vector grid or Houdini's vel.x/.y/.z), as Karma's Lookup Offset. | `blur` vs `sharp` (`eldr:volumeVelocityBlur` off) |
| `2026-10-29_26_volume_multiple_scattering` | Light bouncing more than once inside smoke, set with Karma's Volume Limit (`karma:object:volumelimit`) and fitted to Karma's gain. | `volumeLimit1` vs `volumeLimit4` vs `volumeLimit16` |
| `2026-10-30_12_fog` | Height fog with light shafts from the sun and spot lights (`eldr:fog`). | `fog` vs `fogOff`, `fogDense` |

## Cameras

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_02_velocity_blur` | Objects that only author `velocities` (or instance velocities) blur when their velocity blur is on, as Karma's Velocity Blur. | `blurOn` vs `blurOff` (`eldr:motionBlur`), `blurFinal`, `cameraBlur` |
| `2026-10-29_10_reflection_motion_blur` | Planar and traced reflections blur with motion blur, rendered at each film sample's shutter time. | `planar` vs `frozen` (`eldr:planarMotionBlur` off), `traced` (`eldr:rtReflections`) |
| `2026-10-30_05_camera_exposure` | The USD camera's exposure (exposure, ISO, time, f-stop, responsivity) scales the image as in Karma (`eldr:cameraExposure`). | `camera` vs `ignored` |
| `2026-10-30_14_depth_of_field` | The camera's f-stop and focus distance blur what is out of focus, with true bokeh over the film (`eldr:depthOfField`). | `lens` vs `dofOff` |
| `2026-10-30_17_tonemapping` | The display tonemapper: ACES, AgX or none (`eldr:tonemapper`). | `aces` (default) vs `agx` vs `linear` |
| `2026-10-30_24_lens_distortion` | The camera's `lensDistortion:*` bows straight lines, as HdCamera and a Karma Physical Lens describe it (`eldr:lensDistortion`). | `lens` vs `distortionOff` |
| `2026-10-30_25_bloom` | A glow around very bright pixels (`eldr:bloomIntensity`). | `bloom` (0.4) vs `bloomOff` (0) |

## Sampling

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-29_07_oidn_denoiser` | The converged film is denoised by Intel Open Image Denoise, guided by albedo and normal (`eldr:denoise`, or Karma's image filter). | `raw16` vs `denoise16`, `karmaImageFilter16` (Karma's denoiser string), `raw64` |
| `2026-10-29_11_effect_denoise` | The final gather, traced shadows and traced reflections are each filtered in their own signal during the first film samples. | `raw4` vs `denoise4`, `raw16` vs `denoise16`, `raw64` vs `denoise64`, `noFade64` |
| `2026-10-29_12_quality_multipliers` | More rays for the noisy component (diffuse, light, reflection) instead of more film samples, as Karma's quality controls. | `default`, `diffuse2`, `diffuse2LightHalf`, `karmaNames` (Karma's names), `allHalf` |
| `2026-10-31_01_refraction_quality` | More paths through rough glass instead of more film samples (`eldr:refractQuality`, Karma's `karma:object:refractquality`). | `refractQuality1` vs `refractQuality4` |

## AOVs

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_05_houdini_render_vars` | A Render Product's vars (beauty, CDA, N, depth) show in Houdini's Render Outputs menu and husk writes them to one EXR. | `default` (writes `renderVars.exr`) vs `viewport` (`houdini:viewport` flag) |
| `2026-10-28_06_holdouts` | Objects set as holdouts are cut out of the image as mattes, and phantom objects are hidden from the camera but still cast shadows and show in reflections. | `holdoutScene` vs `holdoutsOff` (`eldr:holdouts`) |
| `2026-10-29_06_karma_aovs` | The render vars a Karma Render Settings LOP writes (light groups, diffuse and glossy splits, transmission, subsurface, coat, volume, shadow mattes) come out of Eldr by the same names. | `default` vs `noHoldouts` (`eldr:holdouts` off) |
| `2026-10-29_14_aov_filters` | Each render var can ask for its own sample filter (min, max, closest, farthest, nearest, box, Gauss), as Karma's Sample Blending. | `default` (writes `aovFilters.exr`), `max` (writes `aovFiltersMax.exr`), `off` (`eldr:aovFilters` off) |
| `2026-10-30_15_cryptomatte` | Cryptomatte object and material layers in the EXR, soft (several ids per pixel) or one id per pixel (`eldr:cryptomatteSoft`). | `crypto` vs `cryptoHard` |

## Performance

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_01_subdivision_levels` | Catmull-Clark meshes are refined to the host's level, capped by `eldr:maxRefineLevel`; a mesh can set its own level or opt a polygon mesh in. | `level3` vs `level1` (`eldr:maxRefineLevel`), `subdivOff` (`eldr:subdivision` off) |
| `2026-10-28_07_shadow_cull` | Tiny instances are skipped in the far sun-shadow cascades to save time (`eldr:shadowCullPixels`). | `cull05` (default) vs `cull8` vs `cullOff` |
| `2026-10-29_02_offline_batch` | Offline renders run film samples back to back inside one execute, removing the app's wait between samples (`eldr:offlineBatchSeconds`). | `batched` vs `oneSamplePerExecute` |
| `2026-10-29_04_screen_dicing` | Subdivision meshes get the refine level their size on screen needs, as Karma's Dicing Quality (`eldr:screenDicing`). | `dicing` vs `fixed` |
| `2026-10-29_19_texture_compression` | 8-bit textures are block-compressed on the GPU (BC7, BC4, BC5) and cached on disk, to save memory (`eldr:textureCompression`). | `compressed` vs `exact` |
| `2026-10-29_20_volume_light_shadow_stride` | Volumes take a local light's shadow every second march step instead of every step, for the same image (`eldr:volumeLocalShadowStride`). | `stride2` (default) vs `stride1` vs `stride4` |
| `2026-10-29_23_adaptive_tiles` | With adaptive sampling, tiles whose pixels have all stopped skip shading and rays (`eldr:adaptiveTiles`). | `adaptiveTiles256` vs `adaptiveFull256` |
| `2026-10-29_25_refraction_clip` | With traced refraction, a pixel's first ray looks for glass only, up to the opaque depth, for the same image (`eldr:rtRefractionClip`). | `clip` (default) vs `noClip` |
| `2026-10-30_02_reflection_roulette` | Dim traced reflections are traced in only some film samples, weighted to match (`eldr:rtReflectionRoulette`). | `roulette` vs `everySample` |
| `2026-10-30_03_volume_shadow_grid` | Each volume's optical depth towards each light is built once on a grid instead of a shadow march per step (`eldr:volumeShadowGrid`). | `grid` (64, default) vs `exact` (0) vs `grid32` |
| `2026-10-30_04_light_sampling` | Traced hits sample one light stochastically and traced shadows skip lights that give a pixel nothing (`eldr:rtLightSampling`). | `sampled` vs `everyLight` |
| `2026-10-30_20_instancing` | Nested point instancers and native instances, with per-instance culling by screen size (`eldr:cullPixels`). | `instances` vs `cullSmall` |

## Viewport

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_08_render_stats` | Eldr's render statistics (renderer, memory, time) show in the Render Gallery's statistics panel and in husk EXRs as `husk:render_stats`. | `default` only (no A/B: the feature is metadata) |
| `2026-10-30_01_moving_camera` | While the camera moves, Eldr renders smaller (to a GPU frame budget) with one shadow ray and paused GI; after the stop, quality steps up over a few frames and the film starts from the TAA image. | `moving` (the defaults) vs `full` vs `abrupt` |
