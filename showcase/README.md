# Eldr showcase scenes

Small scenes to try Eldr features yourself. Each folder has a `scene.usda` with several `/Render/*` Render Settings prims (A/B toggles) and a `README.md`.

Every folder also has a `scene.hip` with the standard `eldr_default` (Eldr, nothing set), `eldr_final` (the final-render settings ticked) and `karma` (for Karma) render settings nodes, each with a USD Render ROP, and the same three prims in `scene.usda`.

Every new feature adds a folder here, named `date_NN_name`, so they sort old to new.

| Date | Folder | Feature | Settings |
|---|---|---|---|
| 2026-10-28 | `2026-10-28_01_memory_subdivision` | Subdivision refine levels, polygon meshes as subdivision, memory | `eldr:maxRefineLevel`, `eldr:subdivision`, `primvars:eldr:refineLevel`, `renderAsSubdivision` |
| 2026-10-28 | `2026-10-28_02_velocity_blur` | Velocity and instance blur, eldr over karma | `eldr:motionBlur`, `primvars:eldr:object:vblur`, `instance_vblur` |
| 2026-10-28 | `2026-10-28_03_emitters_shadows_reflections` | Traced shadows and reflections, emissive lights | `eldr:rtShadows`, `rtReflections`, `rtHitShadows`, `emissiveLights` |
| 2026-10-28 | `2026-10-28_04_gi_final_gather` | GI probes, final gather, SSGI | `eldr:gi`, `ssgi`, `finalGather`, `giRelocation`, `finalGatherDistance` |
| 2026-10-28 | `2026-10-28_05_houdini_render_vars` | Render Vars in the Houdini viewport and husk | Render Product and Render Var prims, `houdini:viewport` |
| 2026-10-28 | `2026-10-28_06_eldr_geometry_tab` | Per-object eldr: attributes over karma:, image group | `holdoutmode`, `rendervisibility`, `width_scale`, `curve_style`, `point_style`; `eldr:productLinear`, `bloomIntensity` |
| 2026-10-28 | `2026-10-28_07_shadow_cull` | Small-object culling in sun shadows | `eldr:shadowCullPixels` |
| 2026-10-28 | `2026-10-28_08_render_stats` | Eldr's statistics in the Render Gallery panel, `husk:render_stats` in EXRs | `husk.metadata` in `UsdRenderers.json` |
| 2026-10-29 | `2026-10-29_01_quick_fixes` | Colour limit, traced reflections, UsdPreviewSurface opacityMode, displacement graph bound | `eldr:colorLimit`, `indirectClamp`, `rtReflections`, `opacityMode`, `eldr:displacementBound` (material input) |
| 2026-10-29 | `2026-10-29_02_offline_batch` | husk and usdrecord: film samples back to back in one execute | `eldr:offlineBatchSeconds` |
| 2026-10-29 | `2026-10-29_03_planar_blur_lighttex` | Blurred planar mirrors, light textures in traced paths | `eldr:planarBlur`, `eldr:lightTextures` |
| 2026-10-29 | `2026-10-29_04_screen_dicing` | Subdivision levels by size on screen as Karma's dicing quality | `eldr:screenDicing`, `primvars:eldr:object:dicingquality` |
| 2026-10-29 | `2026-10-29_05_coloured_volume_shadows` | Coloured smoke casts coloured shadows (maps, traced, half resolution); multiple scattering fitted to Karma's Volume Limit | `eldr:volumeShadows`, `eldr:rtShadows`, `eldr:volumeResolution`, `karma:object:volumelimit` |
| 2026-10-29 | `2026-10-29_07_oidn_adaptive_tiles` | Intel Open Image Denoise on the converged film, raw beauty AOV, adaptive sampling skipping stopped tiles | `eldr:denoise`, `karma:global:imagefilter`, `eldr:beautyRaw` AOV, `eldr:adaptiveTiles` |
| 2026-10-29 | `2026-10-29_15_light_linking_ies` | UsdLux light and shadow linking (Houdini Light Linker), IES profiles | `eldr:lightLinking`, `eldr:lightIes`; `collection:lightLink`, `collection:shadowLink`, `shaping:ies:*` |
| 2026-10-29 | `2026-10-29_13_caustics` | Caustics: photons through a glass ball onto the floor, sharper every sample; caustic AOV | `eldr:caustics`, `eldr:causticsPhotons`, `primvars:karma:object:causticsenable` |
| 2026-10-29 | `2026-10-29_06_light_groups` | Karma's AOVs: light groups, transmission, subsurface, coat, volume, shadow mattes | `inputs:karma:light:lpetag`, `eldr:holdouts`; LPE render vars |
| 2026-10-29 | `2026-10-29_09_transparency_order` | Glass and other translucent meshes in one depth order; smoke dimmed by every pane in front of it | `eldr:oit`, `eldr:refractionLayers`, `eldr:volumeUnderTransparency`, `eldr:rtRefraction` |
| 2026-10-29 | `2026-10-29_08_volume_karma_defaults` | Volumes without a material scatter as Karma's (white, no absorption, isotropic; dome from its upper half) | `eldr:volumeAnisotropy`, `karma:object:volumelimit` |
| 2026-10-29 | `2026-10-29_11_effect_denoise` | Ray-traced effects (final gather, shadows, reflections) denoised per signal, fading out over the film | `eldr:effectDenoiseSamples`, `eldr:rtShadowDenoise`, `eldr:rtReflectionDenoise`, `eldr:finalGatherDenoise` |
| 2026-10-29 | `2026-10-29_12_quality_multipliers` | Per-component quality as in Karma: more rays for the noisy component (the final gather) instead of more film samples | `eldr:diffuseQuality`, `eldr:lightQuality`, `eldr:reflectQuality`, `karma:object:diffusequality` |
| 2026-10-29 | `2026-10-29_14_aov_filters` | Per-AOV sample filters: min, max, closest, farthest, nearest, box, Gauss per render var (Karma's Sample Blending) | `driver:parameters:aov:eldr:filter`, `driver:parameters:aov:karma:filter`, `eldr:aovFilters` |
| 2026-10-29 | `2026-10-29_16_mtlx_coverage` | MaterialX compositing nodes, glTF PBR / Disney / unlit surfaces, Karma's Ramp Const, Hex Tiled Texture and Voronoi | `eldr:materialGraphs`, `eldr:proceduralTextures` |
| 2026-10-29 | `2026-10-29_17_karma_colour_limit` | Karma's colour limit on seen emitters and on emissive-mesh light; full-HDR emitters for comp | `eldr:colorLimit`, `eldr:indirectClamp` |
| 2026-10-29 | `2026-10-29_18_volume_motion_blur` | Volumes blur along their `vel` field (vector VDB or Houdini's vel.x/.y/.z), as Karma's Lookup Offset | `eldr:volumeVelocityBlur`, `karma:object:vblur`, `karma:object:volumevblurscale` |
| 2026-10-29 | `2026-10-29_10_reflection_motion_blur` | Planar and traced reflections blur with motion blur time samples; probes stay at the frame's time | `eldr:planarMotionBlur`, `eldr:rtReflections` |
| 2026-10-29 | `2026-10-29_19_texture_compression` | 8-bit textures block-compressed (BC7, BC4, BC5) and cached on disk, against the exact texels | `eldr:textureCompression` |
| 2026-10-29 | `2026-10-29_20_rt_volume_cost` | Cheaper traced refraction (first ray looks for glass only) and volume local-light shadows (a tap every second step), same image | `eldr:rtRefractionClip`, `eldr:volumeLocalShadowStride` |
| 2026-10-30 | `2026-10-30_01_moving_camera` | Moving camera mode: half size, one shadow ray, GI paused while moving; full settings back on the first still frame | `eldr:movingRenderScale`, `eldr:movingShadowRays`, `eldr:movingGiPause` |
| 2026-10-30 | `2026-10-30_02_reflection_roulette` | Dim traced reflections trace in a share of the film's samples per 8x8 block, weighted to match: same converged image, crowd 249 to 146 ms per sample | `eldr:rtReflectionRoulette` |
| 2026-10-30 | `2026-10-30_03_volume_shadow_grid` | Volume shadow grids: three volumes, sun, dome and three sphere lights, 3x cheaper, same image | `eldr:volumeShadowGrid` |
| 2026-10-30 | `2026-10-30_04_light_sampling` | Hits trace one stochastic light, traced shadows skip lights that give a pixel nothing and share samples over 4 lights: same converged image, emissive RT 147 to 78 ms per sample | `eldr:rtLightSampling`, `eldr:finalGatherRoulette` |
| 2026-10-30 | `2026-10-30_05_camera_exposure` | The USD camera's exposure (exposure, ISO, time, f-stop, responsivity) scales the beauty and light AOVs, as Karma | `eldr:cameraExposure` |

Open: `usdenv usdview --renderer Eldr showcase\<folder>\scene.usda`, then pick a settings prim; or `husk -R Eldr -s /Render/<name> <scene.usda>`.
