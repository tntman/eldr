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
| New or changed setting | One line in `hydra/renderSettings.def`, in its group: name, usdview label, C++ default. That makes the token (`HdVkRenderSettingsTokens->name`), the descriptor, the usdview group and Houdini's parm. Read the value in `hydra/renderPass.cpp`. Then its row in `settings.md`, same group and place, key in backticks, Default column as the renderer's (`on`, `int 4`, `float 0.5`, `string aces`). ctest `docs` fails on missing, unknown, misplaced rows or a wrong default; the tooltip is the row's "What it does". |
| New feature | Add it to the matching section of `features.md` with setting names, requirements and a measured cost. Add primvars and attributes to "USD authoring conventions". Update Eldr's column in `comparison.md`. |
| Code shortcut (`ponytail:` comment) | Add a row to "Known limitations". Searching the code for `ponytail:` finds them. |

Numbers are GPU milliseconds on a GTX 1080 Ti unless stated.

## Tests

ctest labels: `slow` (over about a minute on a 1080 Ti: vkrtest, aov_sums, motion, lighting, traced_media, screenspace, displacement_graphs, exr, procedural, karma_golden) and `fast` (the rest, about 7 minutes). Lanes run `ctest -j1 -L fast` plus the slow tests of what they changed (`-R`); the integrator runs everything once after the merges. Always `-j1`: the tests share the GPU.

