# Eldr documentation

Eldr is a Hydra 2.0 Vulkan render delegate for OpenUSD 26.08, for fast raster renders to EXR.

| Page | Contents |
|---|---|
| goals | What Eldr is for and the ground rules |
| features | How Eldr works: pipeline, film, lighting, GI, reflections, transparency, volumes, materials, geometry, camera, output, authoring conventions, limitations |
| settings | Every `eldr:` render setting: type, default, effect, cost |
| comparison | Feature table: Eldr vs EEVEE and Unreal |
| houdini | Houdini 22 build, install and use |
| testing | ctest, and the Karma golden references (`tests/karma_refs.py`) |

## Keeping the docs current

| Change | Do |
|---|---|
| New or changed setting | Edit its row in `settings.md`, in the table of its `GROUPS` group (`usdview/hdvkUsdview/settings.py`). Key in backticks. ctest `docs` fails on missing or unknown keys. |
| New feature | Add it to the matching section of `features.md` with setting names, requirements and a measured cost. Add primvars and attributes to "USD authoring conventions". Update Eldr's column in `comparison.md`. |
| Code shortcut (`ponytail:` comment) | Add a row to "Known limitations". Searching the code for `ponytail:` finds them. |

Numbers are GPU milliseconds on a GTX 1080 Ti unless stated.
