# Dicing by screen size

Five loop-subdivision octahedra from near to far. In final quality (husk, or `eldr:finalQuality`) `eldr:screenDicing` gives each mesh the refine level its size on screen needs, as Karma's Dicing Quality (`primvars:karma:object:dicingquality` or `primvars:eldr:object:dicingquality`, 0.5: about a polygon per 4 pixels): the near one at level 3, the far ones lower. The farthest has its own quality 4, so it refines more than its size alone would ask.

- `/Render/dicing`: screen dicing on (the default in final quality).
- `/Render/fixed`: `eldr:screenDicing` off: every mesh at the host's level (husk's `--complexity veryhigh`: 3).

The pictures should look the same at the render resolution; the triangle count drops (usdview's GPU Stats, the stats panel in Houdini, or `trianglesTotal` in husk's `husk:render_stats` EXR metadata). Try `primvars:eldr:object:dicingdepthmin` / `dicingdepthmax` on one mesh, or a fixed `primvars:eldr:refineLevel` (wins over screen dicing).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_04_screen_dicing\scene.usda`, Complexity Very High, then set a settings prim active (right-click). The interactive view re-dices once the camera stops.
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer with Final Quality; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_04_screen_dicing/scene.usda`

Settings prims: `/Render/dicing`, `/Render/fixed`.
