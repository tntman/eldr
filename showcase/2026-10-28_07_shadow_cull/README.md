# Shadow small-object culling

A ground plane and 1600 small posts (a PointInstancer) far from the camera, under a low sun. Far sun-shadow cascades have big texels, so tiny instances can be skipped there to save time.

- `/Render/cull05`: `eldr:shadowCullPixels` 0.5 (default).
- `/Render/cull8`: 8: more posts lose their shadows in the far cascades. Look at the distant rows.
- `/Render/cullOff`: 0, nothing culled (slowest, all shadows).

Frame time shows the difference better than the picture; use usdview's frame time or husk's log. The effect is biggest in dense crowd scenes: the crowd scenes in `E:\usd-testscenes` show it best (that folder is not part of the repo).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_07_shadow_cull\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_07_shadow_cull/scene.usda`

Settings prims: `/Render/cull05`, `/Render/cull8`, `/Render/cullOff`.
