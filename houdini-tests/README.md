# Houdini test scenes

One `.hip` per item of the Houdini checklist (2026-10-30). Each has a sticky note saying what to check, the standard render settings nodes `eldr_default` (Eldr, nothing set), `eldr_final` (each final-render setting ticked) and `karma` (Karma Render Settings to match), the item's own A/B nodes, and a USD Render ROP per node (`render_<node>`; Eldr's run `$ELDR_HUSK`). Pick a node, render its ROP; images go to `render/`. Choose Eldr in the viewport's renderer menu yourself.

Built by `hython houdini/make_houdini_tests.py [name ...]` (with Eldr's Houdini package). Scenes come from `tests/scenes` by `$HIP`-relative paths; crowd_9k only by its absolute path `E:/usd-testscenes/crowd_9k/crowd_9k_v001.usd`. Pyro and SOP volumes are SOP nodes, cooked when opened (no caches).

| File | Checks | A/B nodes |
|---|---|---|
| `01_camera_switch.hip` | Switching to /cameras/cam2 and the free camera restarts the film without smear | |
| `02_moving_camera_crowd.hip` | crowd_9k moving camera: half size while moving, sharp when still | `eldr_moving1` (scale 1) |
| `03_light_linker.hip` | Light Linker LOP: key light off the middle ball, fill shadow off the left ball | `eldr_nolinking` |
| `04_ies.hip` | IES profile (`shaping:ies:file`) on a Houdini light, direction and rotation | `eldr_noies` |
| `05_colour_limit.hip` | Karma Color Limit 20 vs 0 on visible emitters | `karma_limit20_eldr`, `karma_limit0_eldr`, `karma_limit0_karma` |
| `06_karma_quality.hip` | Karma Diffuse/Reflect/Light/SSS/Volume Quality change Eldr's noise | `karma_quality_high_eldr`, `karma_quality_low_eldr`, `karma_quality_high_karma` |
| `07_caustics.hip` | Karma Enable Caustics on the ball + Contributes Caustics on the sun | `eldr_nocaustics` |
| `08_houdini_materials.hip` | Material Palette materials (metals, redvelvet, glass, lacquer) vs Karma; Silk and SoapBubble are not in H22's palette | |
| `09_denoise.hip` | Karma Denoiser (OIDN) and `eldr:denoise`; Houdini's OIDN loads | `karma_oidn_eldr`, `eldr_denoise_off`, `eldr_denoise_on` |
| `10_pyro_velocity_blur.hip` | Pyro sim with `vel` + Karma Velocity Blur, frame 24 | `eldr_novblur` |
| `11_nameless_sop_volume.hip` | A Volume SOP without a name (field_0) renders as density | |
| `12_coloured_smoke_shadows.hip` | Coloured smoke (density + Cd) casts a tinted shadow | `eldr_rtshadows` |
| `13_husk_progress_timelimit.hip` | husk progress with Offline Batch Seconds; the ROP's time limit (5 s) | `eldr_batch`, `eldr_timelimit` |
| `14_aov_menu_sample_blending.hip` | Render Outputs menu (depth, N, ids, light group, LPE splits, caustics, beautyRaw); Sample Blending min on `depth_min` | |
