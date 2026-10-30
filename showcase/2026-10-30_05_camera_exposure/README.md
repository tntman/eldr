# Camera exposure

<!-- summary -->
The USD camera's exposure (exposure, ISO, time, f-stop, responsivity) scales the image as in Karma (`eldr:cameraExposure`).

- **Compare:** open `showcase/2026-10-30_05_camera_exposure/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `camera` vs `ignored`.
- **Look at:** The whole image: 4x brighter (linear) with the camera's exposure.
<!-- summary -->

Eldr applies the USD camera's exposure as Karma does: the image times `HdCamera::GetLinearExposureScale()`, which UsdGeomCamera computes from `exposure`, `exposure:iso`, `exposure:time`, `exposure:fStop` and `exposure:responsivity`. Beauty and light AOVs (emission, the diffuse and glossy splits, light groups) scale; depth, normals, ids and albedo do not.

What to look at: the Cornell box's `/cam` here is exposure 1 at ISO 200 (4x). `/Render/camera` (the default) is 4x brighter (linear, before tonemapping) than `/Render/ignored` (`eldr:cameraExposure` off). Colour limits clamp the exposed values, as in Karma: on `tests/scenes/emissive.usda`, whose light is mostly clipped indirect light, 4x exposure brightens it only about 1.03x in both renderers.

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_05_camera_exposure\scene.usda`, then pick a settings prim, or flip Camera Exposure in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/camera scene.usda`.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_05_camera_exposure\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-30_05_camera_exposure/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
