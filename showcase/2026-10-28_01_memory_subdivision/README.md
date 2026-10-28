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

Settings prims: `/Render/level3`, `/Render/level1`, `/Render/subdivOff`.
