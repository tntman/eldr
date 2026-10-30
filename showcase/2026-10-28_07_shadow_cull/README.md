# Small-object culling in sun shadows

<!-- summary -->
Tiny instances are skipped in the far sun-shadow cascades to save time (`eldr:shadowCullPixels`).

- **Compare:** open `showcase/2026-10-28_07_shadow_cull/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `cull05` (default) vs `cull8` vs `cullOff`.
- **Look at:** The distant rows of posts: at 8 some lose their shadows; at the default the picture equals `cullOff`, only faster.
<!-- summary -->

A ground plane and 1600 small posts (a PointInstancer) far from the camera, under a low sun. Far sun-shadow cascades have big texels, so tiny instances can be skipped there to save time.

- `/Render/cull05`: `eldr:shadowCullPixels` 0.5 (default).
- `/Render/cull8`: 8: more posts lose their shadows in the far cascades. Look at the distant rows.
- `/Render/cullOff`: 0, nothing culled (slowest). At the default the picture is the same as with culling off here; 8 is where the far rows change.

Frame time shows the difference better than the picture; use usdview's frame time or husk's log. The effect is biggest in dense crowd scenes: the crowd scenes in `E:\usd-testscenes` show it best (that folder is not part of the repo).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_07_shadow_cull\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_07_shadow_cull/scene.usda`

Settings prims: `/Render/cull05`, `/Render/cull8`, `/Render/cullOff`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
