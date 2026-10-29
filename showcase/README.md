# Eldr showcase scenes

Small scenes to try Eldr features yourself. Each folder has a `scene.usda` with several `/Render/*` Render Settings prims (A/B toggles) and a `README.md`.

Every new feature adds a folder here, named `date_NN_name`, so they sort old to new.

| Date | Folder | Feature | Settings |
|---|---|---|---|
| 2026-10-28 | `2026-10-28_01_memory_subdivision` | Subdivision refine levels, polygon meshes as subdivision, memory | `eldr:maxRefineLevel`, `eldr:subdivision`, `primvars:eldr:refineLevel`, `renderAsSubdivision` |
| 2026-10-28 | `2026-10-28_02_velocity_blur` | Velocity and instance blur, eldr over karma | `eldr:motionBlur`, `primvars:eldr:object:vblur`, `instance_vblur` |
| 2026-10-28 | `2026-10-28_03_emitters_shadows_reflections` | Traced shadows and reflections, emissive lights | `eldr:rtShadows`, `rtReflections`, `rtHitShadows`, `finalShadows`, `emissiveLights` |
| 2026-10-28 | `2026-10-28_04_gi_final_gather` | GI probes, final gather, SSGI | `eldr:gi`, `ssgi`, `finalGather`, `giRelocation`, `finalGatherDistance` |
| 2026-10-28 | `2026-10-28_05_houdini_render_vars` | Render Vars in the Houdini viewport and husk | Render Product and Render Var prims, `houdini:viewport` |
| 2026-10-28 | `2026-10-28_06_eldr_geometry_tab` | Per-object eldr: attributes over karma:, image group | `holdoutmode`, `rendervisibility`, `width_scale`, `curve_style`, `point_style`; `eldr:productLinear`, `bloomIntensity` |
| 2026-10-28 | `2026-10-28_07_shadow_cull` | Small-object culling in sun shadows | `eldr:shadowCullPixels` |
| 2026-10-28 | `2026-10-28_08_render_stats` | Eldr's statistics in the Render Gallery panel, `husk:render_stats` in EXRs | `husk.metadata` in `UsdRenderers.json` |
| 2026-10-29 | `2026-10-29_01_quick_fixes` | Colour limit, final-quality traced reflections, UsdPreviewSurface opacityMode, displacement graph bound | `eldr:colorLimit`, `indirectClamp`, `rtReflections`, `opacityMode`, `eldr:displacementBound` (material input) |
| 2026-10-29 | `2026-10-29_02_offline_batch` | husk and usdrecord: film samples back to back in one execute | `eldr:offlineBatchSeconds` |
| 2026-10-29 | `2026-10-29_03_planar_blur_lighttex` | Blurred planar mirrors, light textures in traced paths | `eldr:planarBlur`, `eldr:lightTextures` |
| 2026-10-29 | `2026-10-29_04_screen_dicing` | Subdivision levels by size on screen in final quality, as Karma's dicing quality | `eldr:screenDicing`, `primvars:eldr:object:dicingquality` |
| 2026-10-29 | `2026-10-29_05_coloured_volume_shadows` | Coloured smoke casts coloured shadows (maps, traced, half resolution); multiple scattering fitted to Karma's Volume Limit | `eldr:volumeShadows`, `eldr:rtShadows`, `eldr:volumeResolution`, `karma:object:volumelimit` |
| 2026-10-29 | `2026-10-29_07_oidn_adaptive_tiles` | Intel Open Image Denoise on the converged film, raw beauty AOV, adaptive sampling skipping stopped tiles | `eldr:denoise`, `karma:global:imagefilter`, `eldr:beautyRaw` AOV, `eldr:adaptiveTiles` |
| 2026-10-29 | `2026-10-29_09_light_linking_ies` | UsdLux light and shadow linking (Houdini Light Linker), IES profiles | `eldr:lightLinking`, `eldr:lightIes`; `collection:lightLink`, `collection:shadowLink`, `shaping:ies:*` |
| 2026-10-29 | `2026-10-29_06_light_groups` | Karma's AOVs: light groups, transmission, subsurface, coat, volume, shadow mattes | `inputs:karma:light:lpetag`, `eldr:holdouts`; LPE render vars |

Open: `usdenv usdview --renderer Eldr showcase\<folder>\scene.usda`, then pick a settings prim; or `husk -R Eldr -s /Render/<name> <scene.usda>`.

