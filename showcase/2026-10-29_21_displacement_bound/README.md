# Displacement bound

<!-- summary -->
A material's displacement shader can state its largest offset (`inputs:eldr:displacementBound`) so culling keeps a surface that a displacement graph moves far.

- **Compare:** open `showcase/2026-10-29_21_displacement_bound/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `culled` (frustum culling on, the default) vs `noCulling` (`eldr:frustumCulling` off).
- **Look at:** The two orange patches lifted 3.3 units from below the floor: only the left one (bound stated) shows when culling is on; both show with `noCulling`.
<!-- summary -->

`tests/scenes/emissive.usda` with two patches 3.3 units below the floor, lifted into the room by a constant MaterialX displacement graph. The left one's displacement shader sets `inputs:eldr:displacementBound = 3.3`; the right one keeps the graph's default bound of |scale| = 1, so its box lies below the picture and frustum culling drops it.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_21_displacement_bound\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_21_displacement_bound/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
