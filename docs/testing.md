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
   final quality, linear colour: no tonemap, bloom or auto exposure; `eldr:domeCameraVisible` off,
   because Karma doesn't show domes to the camera by default) and prints per scene, Eldr / Karma:

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
(transmission), volume, displacement, subsurface.

Baseline of 2026-10-29 (Eldr / Karma 22.0.466): lights 0.99, step4 1.07, glass 0.96, subsurface 0.97,
volume 1.39 (1.12 before lane 30; its mald fell from 0.41 to 0.26 as the clouds now match; lum rose because darker clouds no longer offset the bare ground, see below), mtlx 1.16, cornell 0.76, emissive 1.74, displacement 1.91, sunangle 2.27 (mean luminance
ratio; see `tests/karma_golden.json` for all metrics). In sunangle, the ground (displayColor 0.7, no
material, under sun and sky) is 0.65 in Eldr, which is the Lambert value (0.68), and 0.28 in Karma. The cause is Karma's surface for prims without a material (measured by lane 30): under the same sun a
UsdPreviewSurface with diffuse 0.5 renders equally in both (0.270 Karma, 0.272 Eldr, Lambert 0.274),
while bare displayColor 0.5 gives 0.110 in Karma. Karma does use displayColor, but scaled by about 0.4
with a steeper falloff towards grazing light (about cos^1.45). That is not USD's displayColor fallback,
so Eldr keeps Lambert of displayColor, as Storm does. Measure with materials bound when it matters.

The point is catching regressions and tracking the distance to Karma, not pixel equality: the grids are
coarse enough that sample noise stays well under the tolerances.

Other Karma comparisons: `tests/macbeth_compare.py` (ColorChecker colours, CIEDE2000; ctest `macbeth`
runs its Eldr-only analytic check) and `tests/houdini_dome.py` (hython, dome texture and linear beauty).
