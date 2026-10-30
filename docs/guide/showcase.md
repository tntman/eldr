# Showcase scenes

The `showcase/` folder has one small scene per feature. Open `showcase/<folder>/scene.hip`, compare `eldr_default` (Eldr's defaults) vs `eldr_final` (the final-render settings) vs `karma` (Karma to match), then the feature's own A/B nodes. Each folder's `README.md` says what to look at. Without Houdini, open `scene.usda` in usdview (`usdenv usdview --renderer Eldr showcase\<folder>\scene.usda`) or render a prim with husk.

## Lights

- `2026-10-28_03_emissive_lights`: Emissive meshes as lights
- `2026-10-29_15_light_linking`: Light linking
- `2026-10-29_17_colour_limit`: Colour limit
- `2026-10-29_22_light_textures`: Light textures
- `2026-10-29_24_ies_profiles`: IES profiles
- `2026-10-30_06_many_lights`: Many lights
- `2026-10-30_07_light_shapes`: Light shapes and highlights
- `2026-10-30_08_dome_sun`: Dome sun
- `2026-10-30_09_ambient_occlusion`: Ambient occlusion
- `2026-10-30_10_shadow_modes`: Shadow modes
- `2026-10-30_16_light_path_intensity`: Direct and indirect intensity

## Materials

- `2026-10-28_10_curve_styles`: Per-object curve and point styles
- `2026-10-29_01_opacity_mode`: UsdPreviewSurface opacity mode
- `2026-10-29_09_transparency_order`: Transparency order
- `2026-10-29_16_materialx_nodes`: MaterialX nodes and surfaces
- `2026-10-29_21_displacement_bound`: Displacement bound
- `2026-10-30_13_subsurface`: Subsurface scattering
- `2026-10-30_18_displacement`: Displacement
- `2026-10-30_19_curves_points`: Curves and points
- `2026-10-30_21_textures_colour`: Textures and colour spaces
- `2026-10-30_22_material_lobes`: Material lobes

## Ray tracing

- `2026-10-28_04_gi_final_gather`: Global illumination and final gather
- `2026-10-28_09_traced_reflections`: Ray-traced reflections
- `2026-10-29_03_planar_reflection_blur`: Planar reflection blur
- `2026-10-29_13_caustics`: Caustics
- `2026-10-30_11_screen_space_reflections`: Screen-space reflections
- `2026-10-30_23_reflection_probe`: Reflection probe

## Volumes

- `2026-10-29_05_coloured_volume_shadows`: Coloured volume shadows
- `2026-10-29_08_volume_default_material`: Volumes without a material
- `2026-10-29_18_volume_motion_blur`: Volume motion blur
- `2026-10-29_26_volume_multiple_scattering`: Volume multiple scattering
- `2026-10-30_12_fog`: Height fog

## Cameras

- `2026-10-28_02_velocity_blur`: Velocity motion blur
- `2026-10-29_10_reflection_motion_blur`: Motion blur in reflections
- `2026-10-30_05_camera_exposure`: Camera exposure
- `2026-10-30_14_depth_of_field`: Depth of field
- `2026-10-30_17_tonemapping`: Tonemapping
- `2026-10-30_24_lens_distortion`: Lens distortion
- `2026-10-30_25_bloom`: Bloom

## Sampling

- `2026-10-29_07_oidn_denoiser`: Intel Open Image Denoise
- `2026-10-29_11_effect_denoise`: Per-effect denoise
- `2026-10-29_12_quality_multipliers`: Per-component quality

## AOVs

- `2026-10-28_05_houdini_render_vars`: Render vars in the Houdini viewport
- `2026-10-28_06_holdouts`: Holdouts and phantom objects
- `2026-10-29_06_karma_aovs`: Karma's AOVs and light groups
- `2026-10-29_14_aov_filters`: Per-AOV sample filters
- `2026-10-30_15_cryptomatte`: Cryptomatte

## Performance

- `2026-10-28_01_subdivision_levels`: Subdivision levels
- `2026-10-28_07_shadow_cull`: Small-object culling in sun shadows
- `2026-10-29_02_offline_batch`: Batched film samples in husk and usdrecord
- `2026-10-29_04_screen_dicing`: Dicing by screen size
- `2026-10-29_19_texture_compression`: Texture compression
- `2026-10-29_20_volume_light_shadow_stride`: Volume local-light shadow stride
- `2026-10-29_23_adaptive_tiles`: Adaptive sampling by tiles
- `2026-10-29_25_refraction_clip`: Traced refraction starts at glass
- `2026-10-30_02_reflection_roulette`: Traced reflection roulette
- `2026-10-30_03_volume_shadow_grid`: Volume shadow grids
- `2026-10-30_04_light_sampling`: Traced light sampling
- `2026-10-30_20_instancing`: Instancing

## Viewport

- `2026-10-28_08_render_stats`: Render statistics in Houdini's Render Gallery
- `2026-10-30_01_moving_camera`: Moving camera mode
