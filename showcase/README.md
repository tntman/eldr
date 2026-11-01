# Eldr showcase scenes

One folder per feature, named `YYYY-MM-DD_NN_feature`. Each has a `scene.usda`, a short `README.md` and a generated `scene.hip`.

Every scene has the same three render settings nodes: `eldr_default` (Eldr, nothing set), `eldr_final` (Eldr's final-render settings) and `karma` (Karma to compare), plus the feature's own nodes. Open `scene.hip`, render the nodes and compare.

Without Houdini: `usdenv usdview --renderer Eldr showcase\<folder>\scene.usda` and pick a settings prim, or `husk -R Eldr -s /Render/<name> showcase/<folder>/scene.usda`. `hython houdini/make_showcase_hip.py` rebuilds the `.hip` files.

## Lights

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_03_emissive_lights` | Glowing meshes light the scene like lights, with shadows. | `emissiveLightsOn`, `emissiveLightsOff` |
| `2026-10-29_15_light_linking` | A light can skip objects, and its shadows can skip casters. | `default`, `noLinking`, `traced` |
| `2026-10-29_17_colour_limit` | Karma's colour limit clamps how bright emitters and their bounced light can get; a full-HDR option keeps the emitters unclamped for comp. | `limit20`, `fullHdr`, `off`, `karmaName5` |
| `2026-10-29_22_light_textures` | A rect or disk light with a texture is coloured by it, in direct light, bounce light and reflections. | `lightTextures`, `lightTexturesOff` |
| `2026-10-29_24_ies_profiles` | A light's IES profile shapes its light, as in Karma. | `ies`, `noIes` |
| `2026-10-30_06_many_lights` | Hundreds of small lights are shaded cheaply: each pixel is lit only by the lights that reach it. | `lights`, `clustersOff`, `localLightsOff` |
| `2026-10-30_07_light_shapes` | Sun, sphere, rect, disk and tube lights give highlights of their own shape, and lights set to show their shape appear to the camera. | `shapes`, `shapesVisible` |
| `2026-10-30_08_dome_sun` | The sun in an HDRI becomes a distant light that casts crisp shadows. | `domeSun`, `domeSunOff` |
| `2026-10-30_26_further_sun_shadows` | A second sun, such as the one in an HDRI, casts its own shadows. | `furtherSuns`, `furtherSunsOff`, `furtherSunsTraced` |
| `2026-10-30_09_ambient_occlusion` | The dome's light is darkened in corners and gaps. | `ambient`, `aoOff`, `aoWide` |
| `2026-10-30_10_shadow_modes` | Shadows can be soft or hard, from shadow maps, or traced. | `maps`, `pcss`, `hard`, `traced` |
| `2026-10-30_16_light_path_intensity` | Direct and bounced light can be scaled separately in the final pixel. | `neutral`, `indirectBoost`, `directDim` |

## Materials

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_10_curve_styles` | Each curve or points prim can set its width, tubes or ribbons, and sphere points. | `styles`, `ribbons` |
| `2026-10-29_01_opacity_mode` | A surface set to "presence" fades completely, highlight included; "transparent" keeps the highlight. | `presence`, `opacityModeOff` |
| `2026-10-29_09_transparency_order` | Glass, translucent meshes and smoke are layered in depth order, so what is behind a pane shows through it. | `default`, `sorted`, `oneLayer`, `volumesOverGlass`, `final` |
| `2026-10-29_16_materialx_nodes` | MaterialX compositing nodes, glTF, Disney and unlit surfaces, and Karma's ramp, hex-tiled and Voronoi nodes. | `graphsOn`, `graphsOff`, `proceduralsOff` |
| `2026-10-29_21_displacement_bound` | A displacement shader can state its largest offset so a surface lifted far from its mesh is not culled. | `culled`, `noCulling` |
| `2026-10-30_13_subsurface` | Light scatters under the surface of skin, jade and leaves. | `subsurface`, `subsurfaceOff`, `subsurfaceQuality` |
| `2026-10-30_18_displacement` | Displacement shaders move the surface and cast shadows. | `displacement`, `displacementOff`, `displacementHalf` |
| `2026-10-30_19_curves_points` | Every curve type as tubes or ribbons, points as spheres or discs, and hair shading. | `curves`, `ribbons`, `sprites`, `hairOff` |
| `2026-10-30_21_textures_colour` | Textures in ACEScg and Rec.709, UDIM tiles, and sharp textures at grazing angles. | `textures`, `aniso1` |
| `2026-10-30_22_material_lobes` | Clearcoat, sheen, fuzz, thin film, anisotropic highlights, hair and toon shading. | `lobes`, `lobesOff`, `toon` |
| `2026-10-31_02_karma_default_material` | Objects without a material look as they do in Karma: dim, with a faint white highlight. | `karmaDefault`, `lambertDefault` |
| `2026-10-31_03_houdini_karma_nodes` | Houdini's basic materials that use Karma's own nodes render as in Karma. | `graphsOn`, `graphsOff` |
| `2026-10-31_05_bsdf_graphs` | Surfaces built from MaterialX BSDF nodes (mix, layer, add, multiply), Lama and glTF materials render like Karma. | `graphsOn`, `graphsOff` |

## Ray tracing

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_04_gi_final_gather` | Bounced light from light probes, a traced final gather, or screen-space GI. | `probesOnly`, `finalGather`, `finalGatherNoRelocation`, `finalGatherDistance0`, `finalGatherDistance05`, `denoise0`, `denoise1`, `rays1`, `rays4`, `ssgi`, `giPlusSsgi` |
| `2026-10-28_09_traced_reflections` | Reflections trace the whole scene, including what is behind the camera or off screen. | `traced`, `screenSpace`, `tracedNoHitShadows` |
| `2026-10-29_03_planar_reflection_blur` | A rough mirror floor blurs its reflection by its roughness. | `blurred`, `sharp` |
| `2026-10-29_13_caustics` | Light focused through glass lands as a bright patch on other surfaces. | `caustics`, `noCaustics`, `caustics8`, `causticsFewPhotons` |
| `2026-10-30_11_screen_space_reflections` | Reflections of what is on screen, blurrier on rougher surfaces. | `ssr`, `ssrOff` |
| `2026-10-30_23_reflection_probe` | A captured view of the surroundings supplies reflections where the screen has none. | `probe`, `probeOff` |

## Volumes

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-29_05_coloured_volume_shadows` | Coloured smoke casts coloured shadows. | `maps`, `traced`, `halfResolution`, `shadowsOff` |
| `2026-10-29_08_volume_default_material` | A volume with no material scatters like Karma's default: white, no absorption, even in all directions. | `newPhase`, `oldPhase` |
| `2026-10-29_18_volume_motion_blur` | Smoke blurs along its velocity field, as Karma's volume motion blur. | `blur`, `sharp` |
| `2026-10-29_26_volume_multiple_scattering` | Light bounces more than once inside smoke, set with Karma's Volume Limit. | `volumeLimit1`, `volumeLimit4`, `volumeLimit16` |
| `2026-10-30_12_fog` | Height fog with light shafts from the sun and spot lights. | `fog`, `fogOff`, `fogDense` |

## Cameras

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_02_velocity_blur` | Objects that only author velocities blur along them when velocity blur is on, as in Karma. | `blurOn`, `blurOff`, `blurFinal`, `cameraBlur` |
| `2026-10-29_10_reflection_motion_blur` | Reflections blur with motion blur, like the objects they show. | `planar`, `frozen`, `traced` |
| `2026-10-30_05_camera_exposure` | The camera's exposure, ISO, shutter and f-stop scale the image, as in Karma. | `camera`, `ignored` |
| `2026-10-30_14_depth_of_field` | The camera's f-stop and focus distance blur what is out of focus, with true bokeh. | `lens`, `dofOff` |
| `2026-10-30_17_tonemapping` | The display tonemapper: ACES, AgX or none. | `aces`, `agx`, `linear` |
| `2026-10-30_24_lens_distortion` | The camera's lens distortion bows straight lines. | `lens`, `distortionOff` |
| `2026-10-30_25_bloom` | Very bright pixels glow. | `bloom`, `bloomOff` |

## Sampling

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-29_07_oidn_denoiser` | The finished image is cleaned by Intel Open Image Denoise, guided by albedo and normal. | `raw16`, `denoise16`, `karmaImageFilter16`, `raw64` |
| `2026-10-29_11_effect_denoise` | Bounced light, traced shadows and traced reflections are filtered while few film samples exist, so early images are clean. | `raw4`, `raw16`, `raw64`, `denoise4`, `denoise16`, `denoise64`, `noFade64` |
| `2026-10-29_12_quality_multipliers` | Spend more rays on the noisy part of the image (diffuse, light, reflection) instead of more film samples, as Karma's quality controls do. | `default`, `diffuse2`, `diffuse2LightHalf`, `karmaNames`, `allHalf` |
| `2026-10-31_01_refraction_quality` | More light paths through rough glass give a smoother result without more film samples. | `refractQuality1`, `refractQuality4` |

## AOVs

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_05_houdini_render_vars` | A render product's vars (beauty, CDA, normal, depth) show in Houdini's Render Outputs menu and are written to one EXR. | `default`, `viewport` |
| `2026-10-28_06_holdouts` | Holdout objects are cut out as mattes; phantom objects are hidden from the camera but still shadow and reflect. | `holdoutScene`, `holdoutsOff` |
| `2026-10-29_06_karma_aovs` | The render vars a Karma Render Settings node writes (light groups, diffuse and glossy splits, transmission, subsurface, coat, volume, shadow mattes) come out of Eldr by the same names. | `default`, `noHoldouts` |
| `2026-10-29_14_aov_filters` | Each render var can pick its own sample filter (min, max, closest, farthest, nearest, box, Gauss), as Karma's Sample Blending. | `default`, `max`, `off` |
| `2026-10-30_15_cryptomatte` | The EXR carries Cryptomatte layers for objects and materials, with soft or hard edges. | `crypto`, `cryptoHard` |

## Performance

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_01_subdivision_levels` | Subdivision surfaces are smoothed to the host's level, and a mesh can set its own level or opt in. | `level3`, `level1`, `subdivOff` |
| `2026-10-28_07_shadow_cull` | Tiny instances are skipped in the far sun-shadow cascades to save time. | `cull05`, `cull8`, `cullOff` |
| `2026-10-29_02_offline_batch` | Offline renders run film samples back to back, saving the wait between samples. | `batched`, `oneSamplePerExecute` |
| `2026-10-29_04_screen_dicing` | Subdivision meshes are refined only as much as their size on screen needs. | `dicing`, `fixed` |
| `2026-10-29_19_texture_compression` | 8-bit textures are compressed on the GPU to save memory, at a small cost in detail. | `compressed`, `exact` |
| `2026-10-29_20_volume_light_shadow_stride` | Smoke checks a local light's shadow every second step instead of every step, for the same image in less time. | `stride2`, `stride1`, `stride4` |
| `2026-10-29_23_adaptive_tiles` | Where the image has converged, whole tiles stop being shaded. | `adaptiveTiles256`, `adaptiveFull256` |
| `2026-10-29_25_refraction_clip` | Traced refraction first looks for glass only, for the same image in less time. | `clip`, `noClip` |
| `2026-10-30_02_reflection_roulette` | Dim traced reflections are traced in only some film samples, weighted to match. | `roulette`, `everySample` |
| `2026-10-30_03_volume_shadow_grid` | Each volume's shadow towards each light is built once on a grid instead of marched per step. | `grid`, `exact`, `grid32` |
| `2026-10-30_04_light_sampling` | Traced hits shoot a shadow ray at one chosen light instead of every light. | `sampled`, `everyLight` |
| `2026-10-30_20_instancing` | Nested point instancers and native instances render fast, and tiny instances can be skipped. | `instances`, `cullSmall` |

## Viewport

| Folder | Feature | A/B nodes |
|---|---|---|
| `2026-10-28_08_render_stats` | Eldr's render statistics (renderer, memory, time) show in Houdini's Render Gallery and in husk EXRs. | `default` |
| `2026-10-30_01_moving_camera` | While the camera moves, Eldr renders cheaper and faster, then returns to full quality when it stops. | `moving`, `full`, `abrupt` |
| `2026-10-31_04_rt_temporal_reuse` | While the camera moves, traced shadows and reflections can reuse earlier frames. | `rtTemporalReuse`, `rtTemporalReuseOff` |
