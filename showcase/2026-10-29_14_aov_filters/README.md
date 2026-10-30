# Per-AOV sample filters

The Cornell box with render vars that each ask for their own sample filter: Eldr's `driver:parameters:aov:eldr:filter` (`beauty`, `nearest`, `gauss`, `box`, `min`, `max`, `closest`, `farthest`, optionally a radius in pixels) or Karma's `driver:parameters:aov:karma:filter` (the Render Var's "Sample Blending": `["minmax",{"mode":"max"}]`, `zmin`...). Eldr's wins. Only the film filters (a still image once it renders over film samples); while the camera moves the viewport shows the defaults.

## What to look at

Zoom in to pixels at the silhouettes of the ball and the tall box against the back wall.

- `depth_min` (`aovFilters.exr`) against `depth_max` (`aovFiltersMax.exr`): min takes the near object's depth in every edge pixel (the objects look one pixel fatter), max takes the wall's (one pixel thinner). The `off` product shows Eldr's default, the nearest sample: a hard edge on whichever side the pixel centre fell.
- `Pcam_closest` (Karma's "Sample: Closest"): camera-space position of the sample with the least depth, so the edge pixel's position is the object's, never a blend floating between object and wall.
- `camdepth_farthest`: Hydra's (negative) camera depth of the farthest sample: the wall wins at the edges.
- `N_box` (`box 1`): normals averaged over 3 x 3 pixels; soft against `N` in `aovFiltersMax.exr` (nearest).
- `albedo_nearest`: base colour of one sample per pixel: aliased edges, against the filtered `albedo` in `aovFiltersMax.exr`.
- `directdiffuse` (Karma's `["ubox",{}]`, what Houdini authors on every var): the beauty's filter, so the splits still add up to the beauty.
- `indirectdiffuse_gauss` (`gauss 0.6`): sharper than the beauty's 1.5-pixel Gaussian. The render prints a warning: this split no longer sums to the beauty.
- `primid`: ids are always the nearest sample, whatever the var asks.

## Settings prims

- `/Render/default`: the filters above, `aovFilters.exr`.
- `/Render/max`: depth by max, N and albedo at their defaults, `aovFiltersMax.exr`.
- `/Render/off`: the same vars as default with `eldr:aovFilters` off (all defaults), `aovFiltersOff.exr`.

## Open

- usdrecord: `usdenv usdrecord --renderer Eldr --renderSettingsPrimPath /Render/default showcase\2026-10-29_14_aov_filters\scene.usda out.png` writes `aovFilters.exr` in the current folder; then `/Render/max` and `/Render/off`.
- husk: `husk -R Eldr -s /Render/default showcase/2026-10-29_14_aov_filters/scene.usda`; with `-R BRAY_HdKarma` the Karma-named filters (`depth_max`, `Pcam_closest`, `directdiffuse`) do the same in Karma.
- Houdini: Sublayer LOP on the file; the Render Var LOP's Karma tab shows "Sample Blending". Eldr's own attribute needs an Edit Properties LOP.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
