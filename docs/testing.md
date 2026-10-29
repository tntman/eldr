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
2. `ctest -R karma_golden` (label `slow`) renders the same scenes with Eldr (exr_record.py: final
   quality, linear colour: no tonemap, bloom or auto exposure) and prints per scene, Eldr / Karma:

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

The point is catching regressions and tracking the distance to Karma, not pixel equality: the grids are
coarse enough that sample noise stays well under the tolerances.

Other Karma comparisons: `tests/macbeth_compare.py` (ColorChecker colours, CIEDE2000; ctest `macbeth`
runs its Eldr-only analytic check) and `tests/houdini_dome.py` (hython, dome texture and linear beauty).
