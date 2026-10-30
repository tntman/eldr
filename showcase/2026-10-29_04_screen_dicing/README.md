# Dicing by screen size

Five Catmull-Clark octahedra from near to far (catmullClark, not the referenced mesh's loop: Karma has no loop or bilinear scheme and renders those as the cage, so Karma would not compare). `eldr:screenDicing` (on by default) gives each mesh the refine level its size on screen needs, as Karma's Dicing Quality (`primvars:karma:object:dicingquality` or `primvars:eldr:object:dicingquality`, 0.5: about a polygon per 4 pixels): the near one at level 3, the far ones lower. The farthest has its own quality 4, so it refines more than its size alone would ask.

- `/Render/dicing`: the final-render settings with `eldr:screenDicing` on. `eldr_final` and `eldr_default` dice the same way.
- `/Render/fixed`: the same with `eldr:screenDicing` off: every mesh at the host's level (husk's `--complexity veryhigh`: 3).

The pictures should look the same at the render resolution; the triangle count drops (usdview's GPU Stats, the stats panel in Houdini, or `trianglesTotal` in husk's `husk:render_stats` EXR metadata). Try `primvars:eldr:object:dicingdepthmin` / `dicingdepthmax` on one mesh, or a fixed `primvars:eldr:refineLevel` (wins over screen dicing).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_04_screen_dicing\scene.usda`, Complexity Very High, then set a settings prim active (right-click). The interactive view re-dices once the camera stops.
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_04_screen_dicing/scene.usda`

Settings prims: `/Render/dicing`, `/Render/fixed`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
