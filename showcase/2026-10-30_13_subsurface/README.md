# Subsurface scattering

<!-- summary -->
Light scattering under the surface of skin, jade and leaves (`eldr:subsurface`).

- **Compare:** open `showcase/2026-10-30_13_subsurface/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `subsurface` vs `subsurfaceOff`, `subsurfaceQuality`.
- **Look at:** The soft skin and jade, the ears and the leaf glowing against the sun.
<!-- summary -->

`tests/scenes/subsurface.usda`: a head-like ball with thin ears (standard_surface skin), an OpenPBR jade ball and a thin-walled leaf, the sun low behind them. Eldr's subsurface is a per-channel, depth-aware blur of the diffuse light over the subsurface radius; thin parts transmit the sun.

## What to look at

- `/Render/subsurface` (the default): soft skin and jade (red reaching furthest), the ears and the leaf glowing where the sun shines through.
- `/Render/subsurfaceOff` (`eldr:subsurface` off): hard diffuse, dark ears.
- `/Render/subsurfaceQuality` (`eldr:sssQuality` 4, Karma's `sssquality`): more taps: less noise in the blur.
- Karma's random walk is the reference: Eldr's blur is an approximation (docs/features.md, Spec conformance).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_13_subsurface\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/subsurface scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
