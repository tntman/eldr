# Planar blur, light textures in traced paths

- `/Render/sharp` vs `/Render/blurred`: `eldr:planarBlur` 0 vs 1. A rougher mirror floor blurs its reflection (12-tap disc, radius ~ roughness squared). Change the floor's roughness to compare. Both prims set `eldr:rtReflections` 0: with `eldr:rtReflections` on (as in `eldr_final`) traced reflections replace the planar mirror, and the blur setting has nothing to change.
- Light textures also tint traced hits, probes/GI, fog and volumes (`eldr:lightTextures`): add `inputs:texture:file` to a RectLight and turn on GI or fog to see the pattern in bounce light.
- Open: `usdenv usdview --renderer Eldr showcase\2026-10-29_03_planar_blur_lighttex\scene.usda`, or `husk -R Eldr -s /Render/blurred scene.usda`.
- Not supported: refraction in planar mirrors, contact-hardening blur.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
