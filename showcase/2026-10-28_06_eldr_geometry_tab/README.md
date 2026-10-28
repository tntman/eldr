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

Settings prims: `/Render/holdoutScene`, `/Render/holdoutsOff`, `/Render/curves`, `/Render/curvesTonemapped`, `/Render/curvesBloom`.
