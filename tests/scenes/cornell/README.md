# Cornell box gallery scene

Credits: the Cornell box is Pablo Delgado Krämer's CC0 asset from [pablode/usd-assets](https://github.com/pablode/usd-assets); `cornell.usda`, the variants, `fog.vdb` and `beam.ies` come from [HdUnrealEngine](https://github.com/healkeiser/HdUnrealEngine/tree/main/samples/cornell), also CC0. They are unchanged here. Eldr's own layers, also CC0: `eldr_default.usda` (every effect without ray tracing) and `eldr_final.usda` (the ray-traced final settings) go over `/Render/Settings` for the *Eldr default* and *Eldr final* renders in [docs/gallery.md](../../../docs/gallery.md), which `hython tests/render_gallery.py` renders; the variants `glass`, `subsurface`, `light_texture`, `hair`, `lobes`, `emissive`, `holdout`, `light_groups` and `complex_caustics` were written for this repository in the same style (`complex_caustics.usda` by `make_complex_caustics.py`, which builds its wine glass and rippled slab).

`cornell-box.usda` is Pablo Delgado Krämer's Cornell box, unchanged, from [pablode/usd-assets](https://github.com/pablode/usd-assets/tree/main/cornell-box) (UsdPreviewSurface version), released under CC0 ([LICENSE](LICENSE)). It is modelled from Cornell's measured data, in metres. Houdini's own sample Cornell box is adapted from the same asset.

`cornell.usda` sublayers it and adds what the gallery needs:

- the measured Cornell camera (35 mm lens, 25 x 25 mm film, at 2.78, 2.73, -8.00 m looking +Z);
- the ceiling light as a UsdLux RectLight with the light quad's size, position and radiance (17, 12, 4), replacing the emissive quad so both renderers sample it directly;
- render settings shared by hdUE and Karma: 512 x 512, 512 path-traced samples per pixel, 8 diffuse bounces (Karma's default is 1).

Each file in `variants/` is one small layer over `cornell.usda` that changes one thing. `fog.vdb` (a sphere of fog made in Houdini) and `beam.ies` (a synthetic narrow beam) were made for this repository.

```powershell
hython scripts/render_gallery.py --dev
```

renders every variant with hdUE Lumen, the hdUE Path Tracer and Karma CPU, and writes `docs/images/gallery/`, `docs/gallery.md` and the README gallery.
