# Filters per render var

<!-- summary -->
Each render var can pick its own sample filter (min, max, closest, farthest, nearest, box, Gauss), as Karma's Sample Blending.
<!-- summary -->

A Cornell box with render vars that each use a different filter. Zoom into the silhouette pixels of the ball and the tall box.

- **default**: writes `aovFilters.exr`: `depth_min` makes objects one pixel fatter, `N_box` has soft normals.
- **max**: writes `aovFiltersMax.exr`: `depth_max` makes objects one pixel thinner; `N` and `albedo` use the default filter.
- **off**: filters ignored; every var takes the nearest sample.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

Filters apply to a still image built over film samples.

## Open

Houdini: `scene.hip`. usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_14_aov_filters\scene.usda`, then pick a settings prim. husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_14_aov_filters/scene.usda`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
