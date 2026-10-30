# Testing

Build and run the tests with `ctest -j 1` in the build folder (one GPU). The Python tests run through
`usdenv.cmd` with `HDVK_BUILD` set by ctest. `ctest -LE slow` leaves out the slow ones.

## Karma golden references

Karma is the reference look. Instead of rendering Karma again for every check, Karma references are
rendered once, kept outside the repo, and Eldr is compared against them automatically:

1. `usdenv.cmd python tests\karma_refs.py` renders the references with husk (Karma CPU,
   `BRAY_HdKarma`, Path Traced, 256 samples, 320x180) into `%ELDR_REFS%` (default `E:\eldr-refs`):
   one EXR per scene and `manifest.json` (scene, camera, settings, Houdini version, date, the scene
   file's sha1, seconds). `--scenes a,b` re-renders some; `ELDR_HFS` points at another Houdini.
   Never commit the images.
   Karma takes about 2.5 min for all ten scenes (emissive gets 2048 samples, as its small emitters
   are noisy at 256).
2. `ctest -R karma_golden` (label `slow`, about 1 min) renders the same scenes with Eldr (exr_record.py:
   final quality with GI (`eldr:gi`), linear colour: no tonemap, bloom or auto exposure;
   `eldr:domeCameraVisible` off, because Karma doesn't show domes to the camera by default) and prints
   per scene, Eldr / Karma:

   | Metric | Meaning |
   |---|---|
   | lum | Mean luminance ratio (1 = Karma's exposure) |
   | region | Worst \|ln ratio\| of luminance over a 4x4 grid of regions (where the light lands) |
   | mald | Mean \|ln ratio\| per channel over 32x18 cells (eps 1% of Karma's mean): overall distance, colour included |

   `tests/karma_golden.json` keeps each scene's baseline and the tolerances (global, or a scene's own
   `"tolerance"`). A scene fails when it moves farther from Karma than its baseline plus tolerance;
   moving closer is reported, and `python tests\karma_refs.py --compare --exrinfo <exrinfo.exe> --update`
   records the current values as the new baseline. Scenes without a reference, or whose scene file
   changed since its reference, are skipped; with no references at all the test is skipped (exit 77).

Scenes: cornell, emissive (lighting and exposure), lights (sphere, spot, rect, dome), step4 (HDRI dome
and sun), sunangle (sun and sky), mtlx (MaterialX standard_surface, UsdPreviewSurface), glass
(transmission), volume, displacement, subsurface, caustics, transparency_order, lightlink, ies.

Both renderers get the same wrapper layer, with three settings that keep the comparison about
materials and light rather than defaults (lane 40, 2026-10-29):

- **Eldr renders with GI.** Karma path traces; with GI off Eldr's Cornell ceiling was black (0 against
  0.17-0.32), while its direct light matched the analytic value (back wall 0.621 against 0.629).
- **Karma renders with `karma:object:diffuselimit` 10.** Karma's default is one diffuse bounce, and
  Eldr's GI is every bounce. With 10 the Cornell box matches within 3% (ceiling 0.31 Eldr, 0.30 Karma);
  with Karma's default Eldr is 1.27x brighter there. That is Karma's default, not a lighting difference:
  a user matching Karma's look raises it in Karma or accepts the brighter bounce. The setting is part of
  every reference's hash.
- **Prims without a material get a plain UsdPreviewSurface of their displayColor** in sunangle and
  displacement (`OVERS` in karma_refs.py, also hashed), because Karma's materialless surface differs
  (below).

Karma's colour limit, measured in the emissive scene: Karma clamps every camera sample at
`karma:global:colorlimit` (20), keeping hue (the panel of 40 shows as 20, the (60, 30, 8) bulb as
(20, 10, 2.7)). It reaches emissive meshes only by bounce rays, and clamps each such sample (radiance
times albedo) at the indirect limit, so a white wall gets about 20/0.73 of the panel's 40. Eldr now does
both, from the same settings (`eldr:colorLimit`, `eldr:indirectClamp`; docs/settings.md).

Baseline of 2026-10-29 after lane 40 (Eldr / Karma 22.0.466, mean luminance ratio, before lane 40 in
brackets): cornell 1.03 (0.76), emissive 1.01 (1.74), lights 1.01 (0.99), step4 1.11 (1.07), sunangle
1.01 (2.27), mtlx 1.16, glass 0.95 (0.96), volume 1.40 (1.39), displacement 0.99 (1.91), subsurface
0.96 (0.97), caustics 1.01, transparency_order 1.06 (1.03), lightlink 1.02 (0.95), ies 1.01; see
`tests/karma_golden.json` for all metrics. ies's mald rose from 0.03 to 0.12 with GI: its near-black
corners get 0.0002-0.0003 of bounce light in Eldr against 0.0001 in Karma, under the 1% eps; its lit
cells match within 2%. Before lane 40, in sunangle, the ground (displayColor 0.7, no
material, under sun and sky) is 0.65 in Eldr, which is the Lambert value (0.68), and 0.28 in Karma. The cause is Karma's surface for prims without a material (measured by lane 30): under the same sun a
UsdPreviewSurface with diffuse 0.5 renders equally in both (0.270 Karma, 0.272 Eldr, Lambert 0.274),
while bare displayColor 0.5 gives 0.110 in Karma. Karma does use displayColor, but scaled by about 0.4
with a steeper falloff towards grazing light (about cos^1.45). That is not USD's displayColor fallback,
so Eldr keeps Lambert of displayColor, as Storm does. Measure with materials bound when it matters (the
goldens now do).

The point is catching regressions and tracking the distance to Karma, not pixel equality: the grids are
coarse enough that sample noise stays well under the tolerances.

### usdrecord adds a camera light; pass `--disableCameraLight` to compare

usdrecord (like usdview's default) always adds a headlight unless `--disableCameraLight` is passed, even
when the scene has its own lights. For a renderer other than Storm, Hydra's task controller makes it a
distantLight at the camera: intensity 15000 over a 0.53 degree disc (about 1 unit of irradiance), with no
shadows. Eldr renders it, as Storm renders its simpleLight version (cornell and emissive, both lit by a
RectLight: Storm's mean is 0.64 with it and 0.23 without). husk and karma_refs.py's exr_record.py add no
such light. This difference is intended, not a bug in either path. Lane 51 (2026-10-30) compared the same
Karma-golden wrapper rendered by exr_record and by usdrecord's RenderProduct EXR, Eldr / exr_record:

| Scene | usdrecord | usdrecord `--disableCameraLight` |
|---|---|---|
| VDB smoke (lane 42) | 1.363 (region 0.50) | 1.000 (region 0.000) |
| emissive | 1.208 | 1.000 (0.001) |
| cornell | 1.372 | 1.000 (0.001) |
| volume | 1.209 | 1.000 (0.000) |

With the light off the two paths give the same image (same settings, camera, exposure and samples); the
smoke then matches Karma at lum 0.9999. Always pass `--disableCameraLight` when comparing a usdrecord
render with Karma or husk.

Other Karma comparisons: `tests/macbeth_compare.py` (ColorChecker colours, CIEDE2000; ctest `macbeth`
runs its Eldr-only analytic check) and `tests/houdini_dome.py` (hython, dome texture and linear beauty).
