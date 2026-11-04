# Testing

Build and run the tests with `ctest -j 1` in the build folder (one GPU). The Python tests run through
`usdenv.cmd` with `HDVK_BUILD` set by ctest. `ctest -LE slow` leaves out the slow ones.

## Karma golden references

Karma is the reference look. Karma is rendered once per scene and kept outside the repo; Eldr is then
compared against those images automatically:

1. `usdenv.cmd python tests\karma_refs.py` renders the references with husk (Karma CPU,
   `BRAY_HdKarma`, Path Traced, 256 samples, 320x180) into `%ELDR_REFS%` (default `E:\eldr-refs`):
   one EXR per scene and `manifest.json` (scene, camera, settings, Houdini version, date, the scene
   file's sha1, seconds). `--scenes a,b` re-renders some; `ELDR_HFS` points at another Houdini.
   Never commit the images. Rendering every scene takes a few minutes (the emissive scene uses 2048
   samples, because its small emitters are noisy at 256).
2. `ctest -R karma_golden` (label `slow`, about 1 min) renders the same scenes with Eldr (exr_record.py).
   It uses the final-render settings of `tests/scenes/final_settings.usda` plus GI (`eldr:gi`) and linear
   colour: no tonemap, bloom or auto exposure. `eldr:domeCameraVisible` is off, because Karma doesn't
   show domes to the camera by default. It prints per scene, Eldr / Karma:

   | Metric | Meaning |
   |---|---|
   | lum | Mean luminance ratio (1 = Karma's exposure) |
   | region | Worst \|ln ratio\| of luminance over a 4x4 grid of regions (where the light lands) |
   | mald | Mean \|ln ratio\| per channel over 32x18 cells (eps 1% of Karma's mean): overall distance, colour included |

   `tests/karma_golden.json` keeps each scene's baseline and the tolerances (global, or a scene's own
   `"tolerance"`). A scene fails when it moves farther from Karma than its baseline plus tolerance;
   moving closer is reported, and `python tests\karma_refs.py --compare --exrinfo <exrinfo.exe> --update`
   records the current values as the new baseline. Scenes without a reference, or whose scene file
   differs from the one the reference was rendered from, are skipped; with no references at all the test
   is skipped (exit 77).

Scenes: cornell, emissive (lighting and exposure), lights (sphere, spot, rect, dome), step4 (HDRI dome
and sun), sunangle (sun and sky), mtlx (MaterialX standard_surface, UsdPreviewSurface), glass
(transmission), volume, displacement, subsurface, caustics, transparency_order, lightlink, ies.

The point is catching regressions and tracking the distance to Karma, not pixel equality: the grids are
coarse enough that sample noise stays well under the tolerances.

### What the comparison fixes

Both renderers get the same wrapper layer, with three settings that keep the comparison about
materials and light rather than defaults:

- **Eldr renders with GI.** Karma path traces; with GI off Eldr's Cornell ceiling is black (0 against
  0.17-0.32), while its direct light matches the analytic value (back wall 0.621 against 0.629).
- **Karma renders with `karma:object:diffuselimit` 10.** Karma's default is one diffuse bounce, and
  Eldr's GI is every bounce. With 10 the Cornell box matches within 3% (ceiling 0.31 Eldr, 0.30 Karma);
  with Karma's default Eldr is 1.27x brighter there. That is Karma's default, not a lighting difference:
  to match Karma's look, raise the limit in Karma or accept the brighter bounce. The setting is part of
  every reference's hash.

### Karma's colour limit

Measured in the emissive scene: Karma clamps every camera sample at `karma:global:colorlimit` (20),
keeping hue (the panel of 40 shows as 20, the (60, 30, 8) bulb as (20, 10, 2.7)). It reaches emissive
meshes only by bounce rays, and clamps each such sample (radiance times albedo) at the indirect limit,
so a white wall gets about 20/0.73 of the panel's 40. Eldr does both, from the same settings
(`eldr:colorLimit`, `eldr:indirectClamp`; docs/settings.md).

### Baseline

Mean luminance ratio Eldr / Karma, Houdini 22.0.466: cornell 1.03, emissive 1.01, lights 1.01, step4
1.08, sunangle 1.01, mtlx 1.01, glass 0.93, volume 0.98, displacement 1.00, subsurface 0.94, caustics
0.97, transparency_order 1.05, lightlink 1.02, ies 1.01. `tests/karma_golden.json` has all metrics; volume has its
own tighter tolerance.
ies's mald is 0.12: its near-black corners get 0.0002-0.0003 of bounce light in Eldr against 0.0001 in
Karma, under the 1% eps; its lit cells match within 2%.

### Prims without a material

Karma gives a prim without a material a default material that libBRAY builds in code (no shader file or
setting describes it). Fitted to husk renders of flat patches under a distant light (0-75 degrees from
the normal, viewed from 0-75 degrees) and under a white dome:

- Diffuse: 0.4 x displayColor, Lambert, within 1% at every angle; white (0.4) without a displayColor.
- An untinted highlight: under a white dome 0.033 of the light at any view angle (no Fresnel rise); at
  the mirror direction 0.032 of the light whatever the angle; a Beckmann-like lobe of width 0.29.

Eldr renders the same as a material (`eldr:defaultMaterialKarma`, on by default): diffuse within 1%, the
highlight a GGX lobe of roughness 0.54 and F0 0.034. That highlight is narrower near the mirror
direction, about twice Karma's 30 degrees off it, and up to 3.5x Karma's at a 60 degree mirror
reflection (Schlick Fresnel); under a dome viewed at 60 degrees it is 0.046 against 0.031. The goldens
compare sunangle, displacement and volume with their bare prims: Eldr / Karma 1.01, 1.00 and 0.98
(2.29, 1.88 and 1.31 with the setting off).

### usdrecord adds a camera light; pass `--disableCameraLight` to compare

usdrecord (like usdview's default) always adds a headlight unless `--disableCameraLight` is passed, even
when the scene has its own lights. For a renderer other than Storm, Hydra's task controller makes it a
distantLight at the camera: intensity 15000 over a 0.53 degree disc (about 1 unit of irradiance), with no
shadows. Eldr renders it, as Storm renders its simpleLight version (cornell and emissive, both lit by a
RectLight: Storm's mean is 0.64 with it and 0.23 without). husk and karma_refs.py's exr_record.py add no
such light. This is intended behaviour of each path. The same Karma-golden wrapper rendered by
exr_record and by usdrecord's RenderProduct EXR, Eldr / exr_record:

| Scene | usdrecord | usdrecord `--disableCameraLight` |
|---|---|---|
| VDB smoke | 1.363 (region 0.50) | 1.000 (region 0.000) |
| emissive | 1.208 | 1.000 (0.001) |
| cornell | 1.372 | 1.000 (0.001) |
| volume | 1.209 | 1.000 (0.000) |

With the light off the two paths give the same image (same settings, camera, exposure and samples); the
smoke then matches Karma at lum 0.9999. Always pass `--disableCameraLight` when comparing a usdrecord
render with Karma or husk.

## Side by side with Karma and Blender EEVEE

`tests/compare_renderers.py` renders one scene with Eldr, Karma and Blender EEVEE (the raster
reference: what a good rasterizer reaches with and without its ray tracing) and writes a labelled
mosaic plus numbers:

```
hython tests\compare_renderers.py showcase\<folder>\scene.usda <outdir>
```

- Eldr renders the `eldr_default` and `eldr_final` RenderSettings prims (`--eldr a,b` for others)
  through husk with the Houdini build (`--build`, default `build-h22l`); Karma renders the `karma`
  prim (`BRAY_HdKarma`); EEVEE renders through `tests/eevee_render.py` with its screen-space ray
  tracing on and off, with the Karma prim's camera and resolution.
- `<outdir>/sheet.png` shows them three per row, each labelled with its mean luminance against
  Karma (`x1.00` is Karma's exposure) and its mean absolute difference to Karma as rendered and
  exposure-matched (`d0.12/0.10`, relative to Karma's mean). `stats.txt` has the same numbers.
  EXRs already in `<outdir>` are reused; `--force` renders again.
- Karma hides a dome from the camera unless its Render Light Geometry is on, while Eldr and
  EEVEE show it. The script gives Karma a layer that turns it on for domes that don't author it
  (`--karma-dome-hidden` keeps Karma's default).
- Karma's default diffuse limit is one bounce; Eldr's GI is every bounce. Karma decodes a float
  `.tx` dome texture as sRGB (darker, more saturated); the same pixels as an EXR read as linear in
  all three renderers.

`tests/eevee_render.py` runs on its own too (`blender -b --factory-startup -P
tests/eevee_render.py -- scene.usda out.exr --settings /Render/karma --rt on|off`). Blender's USD
importer reads only UsdPreviewSurface and converts lights in its own units, so the script sets from
the stage:

- MaterialX `standard_surface`, `open_pbr_surface` and `ND_UsdPreviewSurface` materials as a
  Principled BSDF (base, metalness, roughness, IOR, specular, transmission, coat, sheen,
  subsurface, emission, opacity; image inputs a few nodes up become image textures).
- Light power in Karma's units: within 0.5% of Karma on a grey plane for distant (normalized or
  not), sphere, rect, disk and dome lights.
- The dome as the world, in UsdLux's latlong orientation and the dome's transform; bindings and
  smoothing on shape prims (Sphere, Cube); the camera's apertures and offsets.
- With ray tracing off EEVEE reflects only the world, not nearby objects. `--probes auto` puts a
  plane probe on each flat glossy mesh, but in background renders of Blender 5.2 it hides the
  screen-traced reflection and misplaces its own, so it is off by default.
- The camera's exposure scales every light and the world; prims without a material get Karma's
  default material (0.4 x displayColor); a scene with a time range renders its start frame.

Effects EEVEE has no equivalent for (thin film, anisotropy, toon bands, Eldr AOVs, volumes from
VDB) show as differences that are the harness's, not Eldr's.

Other Karma comparisons: `tests/macbeth_compare.py` (ColorChecker colours, CIEDE2000; ctest `macbeth`
runs its Eldr-only analytic check) and `tests/houdini_dome.py` (hython, dome texture and linear beauty).
