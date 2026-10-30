# Subdivision and memory

Eldr refines catmullClark meshes (USD's default scheme when none is authored) to the host's refine level, capped by `eldr:maxRefineLevel`. A mesh can set its own level and can opt a polygon mesh in.

Left to right: `catmullDefault` (scheme unauthored, host level), `catmullLevel1` (`primvars:eldr:refineLevel = 1`), `polygonCage` (scheme none: stays a cube), `polygonAsSubdiv` (scheme none plus `primvars:eldr:renderAsSubdivision = 1`, level 2: rounded without changing the mesh), `dicingCap` (`primvars:eldr:object:dicingdepthmax = 1`, Karma's Dicing Maximum Depth under an eldr name).

## What to look at

- `/Render/level3` vs `/Render/level1`: `eldr:maxRefineLevel` clamps every cube; at 1 they all look like level 1. `/Render/subdivOff` (`eldr:subdivision` 0): all cages.
- Levels come from the host: husk `--complexity low|medium|high|veryhigh` = level 0..3 (usdview: View > Complexity). In Houdini: Level of Detail on the viewport, or per object Subdivision Level / Dicing Maximum Depth. The global cap is `eldr:maxRefineLevel`.
- Memory: husk's peak memory in its log; Eldr prints an `Eldr: loaded in ...` line when the stage is loaded. Compare level 1 and 3 on a heavy scene.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_01_memory_subdivision\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_01_memory_subdivision/scene.usda`

Settings prims: `/Render/level3`, `/Render/level1`, `/Render/subdivOff`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
