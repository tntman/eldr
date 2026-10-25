# Eldr documentation

Eldr (code name HdVk) is a Hydra 2.0 Vulkan render delegate for OpenUSD 26.08,
aimed at fast, high-quality raster renders for VFX (EXR output; seconds per
frame is fine for final quality).

| File | What is in it |
|---|---|
| [features.md](features.md) | How the renderer works, area by area: frame pipeline, film and TAA, lighting, GI, reflections, transparency, volumes, materials, geometry, camera, output (AOVs, EXR, Cryptomatte), performance, USD authoring conventions, known limitations. |
| [settings.md](settings.md) | Every `eldr:` render setting, in the order of usdview's Eldr > Render Settings window: type, default, effect, when to change it, cost. |
| [comparison.md](comparison.md) | Feature-by-feature comparison of Eldr with Blender EEVEE (5.2) and Unreal Engine 5: yes, partial or no, with notes. Update Eldr's column when a feature lands. |
| [houdini.md](houdini.md) | Building Eldr against Houdini 22's USD, installing it in Solaris, what is tested. |
| [eevee-lardomar.md](eevee-lardomar.md) | Design notes on what Eldr can learn from Blender EEVEE (in Swedish). |

## Keeping the docs current

- **New or changed setting**: add or edit its row in `settings.md`, in the
  table of the group that `GROUPS` in `usdview/hdvkUsdview/settings.py` puts it
  in. Give the key in backticks (`eldr:name`). `tests/test_docs.py` (ctest
  `docs`) fails when the renderer exposes a key the page lacks, or the page
  names a key the renderer no longer has.
- **New feature**: describe it in the matching section of `features.md`, with
  the setting names, what it needs and a measured cost if you have one (the
  commit message usually has it). Put authoring conventions (primvars, inputs,
  attributes) in the table under "USD authoring conventions".
- **Known shortcut**: a `ponytail:` comment in the code is a limitation;
  list it under "Known limitations" (`git grep -n "ponytail:"` finds them).
- Numbers are GPU milliseconds on a GTX 1080 Ti unless stated; re-measure
  before quoting a new one.
