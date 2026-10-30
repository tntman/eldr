# Karma's colour limit

The emissive Cornell box (`tests/scenes/emissive.usda`) with three settings prims:

- `/Render/limit20`: Karma's default colour limit of 20. The panel (emission 40) shows as 20 and the bulb
  (60, 30, 8) as (20, 10, 2.7), hue kept. Their light on the walls is clamped as Karma clamps a bounce
  sample (radiance x albedo). This matches Karma's render (karma_golden emissive: 1.01).
- `/Render/fullHdr`: `eldr:colorLimit` 0 with `eldr:indirectClamp` 20. The emitters keep their HDR value
  for comp, and the bounce clamp stays, so the walls match `karma`.
- `/Render/off`: `eldr:colorLimit` 0, which also turns off the bounce clamp. The walls are about 1.4x brighter. The brightest pixel is (20, 10, 2.7) in
  `karma` and (60, 30, 8) in `fullHdr`, and the walls match between those two within 1%.

Look at the panel and the bulb in an EXR viewer or with usdview's pixel probe.

Open: `usdenv usdview --renderer Eldr showcase\2026-10-29_17_karma_colour_limit\scene.usda` and pick a
settings prim. Or run `husk -R Eldr -s /Render/fullHdr showcase\2026-10-29_17_karma_colour_limit\scene.usda`.
In Houdini, set Karma's Color Limit to a large value such as 1e6 for the same full-HDR emitters in Karma (Karma's 0 renders black, not off).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
