# Eldr Geometry tab (per-object attributes)

Per-object `primvars:eldr:object:*` attributes beat `primvars:karma:object:*`, which beat the default. Both are set here with different values so you can see the winner. Houdini's Eldr tab on a Render Geometry Settings LOP authors the eldr names.

Holdout scene (`tests/scenes/holdout.usda` plus overrides):
- `/box`: karma holdoutmode 1, eldr 0. Eldr wins: the box renders normally.
- `/ball`: only karma mode 2: still a matte.
- `/front`: eldr `rendervisibility = "-primary"` (phantom: not seen, still casts its shadow and shows in reflections), karma says `*`.

Curves scene (curves.usda prims referenced 600 units to the side; camera `/curvesScene/cam`):
- `bezier`: eldr width_scale 0.4 vs karma 3: thin. `bspline`: karma 2.5 only: thick.
- `catmullRom`: eldr curve_style 1 (tubes) vs karma 0: tubes.
- `points`: eldr point_style 1 (spheres).

## What to look at

- `/Render/holdoutScene` vs `/Render/holdoutsOff` (`eldr:holdouts`): matte objects cut to alpha 0 or drawn as usual.
- Image group, on the curves camera: `/Render/curves` (Linear Beauty `eldr:productLinear` on), `/Render/curvesTonemapped` (off, ACES) and `/Render/curvesBloom` (off, `eldr:bloomIntensity` 1). Linear Beauty matters for Houdini's viewport and husk products (linear, no tonemap, no bloom); in usdview and usdrecord the tonemapper runs either way, so only the bloom setting changes the picture there.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_06_eldr_geometry_tab\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_06_eldr_geometry_tab/scene.usda`

Settings prims: `/Render/holdoutScene`, `/Render/holdoutsOff`, `/Render/curves`, `/Render/curvesTonemapped`, `/Render/curvesBloom`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
