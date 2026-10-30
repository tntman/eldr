# Lens distortion and depth of field

`tests/scenes/lens.usda` (centimetres: an 85 mm lens at f/1.4 focused at 5 m on spheres from 2 to 20 m) with a barrel distortion added to the camera (`lensDistortion:k1` 0.12, `k2` 0.02, HdCamera's parameters; a Karma Physical Lens is read the same way). Render frame 1: the orange cube moves from frame 2 on (motion blur).

## What to look at

- `/Render/lens` (the default): only the green sphere at 5 m sharp; under the film the camera moves across the aperture every sample (true bokeh with occlusion); the ground's edges bow outwards.
- `/Render/dofOff` (`eldr:depthOfField` off): everything sharp.
- `/Render/distortionOff` (`eldr:lensDistortion` off): straight lines.
- Karma's node reads the same `lensDistortion:*` only through a Physical Lens shader, so the `karma` image is undistorted (tests/scenes/distortion.usda's `/karma` camera has the lens shader).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_14_lens_dof\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/lens scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
