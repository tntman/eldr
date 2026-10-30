# Karma's colour limit (lane 40)

The emissive Cornell box (`tests/scenes/emissive.usda`) with three settings prims:

- `/Render/karma`: Karma's default colour limit of 20. The panel (emission 40) shows as 20 and the bulb
  (60, 30, 8) as (20, 10, 2.7), hue kept. Their light on the walls is clamped as Karma clamps a bounce
  sample (radiance x albedo). This matches Karma's render (karma_golden emissive: 1.01).
- `/Render/fullHdr`: `eldr:colorLimit` 0 with `eldr:indirectClamp` 20. The emitters keep their HDR value
  for comp, and the bounce clamp stays, so the walls match `karma`.
- `/Render/off`: `eldr:colorLimit` 0, which also turns off the bounce clamp. The walls are about 1.4x brighter. Rendered 2026-10-29: the brightest pixel is (20, 10, 2.7) in
  `karma` and (60, 30, 8) in `fullHdr`, and the walls match between those two within 1%.

Look at the panel and the bulb in an EXR viewer or with usdview's pixel probe.

Open: `usdenv usdview --renderer Eldr showcase\2026-10-29_17_karma_colour_limit\scene.usda` and pick a
settings prim. Or run `husk -R Eldr -s /Render/fullHdr showcase\2026-10-29_17_karma_colour_limit\scene.usda`.
In Houdini, set Karma's Color Limit to 0 for the same full-HDR emitters in Karma.
