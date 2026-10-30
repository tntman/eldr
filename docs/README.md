# Eldr documentation

Eldr is a Hydra 2.0 Vulkan render delegate for OpenUSD 26.08, for fast raster renders to EXR.

**User guide** (`guide/`, start here): getting started, rendering in Solaris, lights, materials, sampling and denoising, ray-traced effects, volumes, cameras, AOVs, crowds and performance, viewport, Eldr and Karma, showcase scenes (one folder per feature, by topic), troubleshooting. Plain language, pictures from the showcase scenes, "Where in Houdini" boxes. The pages below are the reference.

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
| User-visible feature | A short plain-language section in the matching `guide/` chapter: what it does, the setting's label, "Where in Houdini", and a before/after picture (add the showcase prims to `site/render_images.py`, run `usdenv.cmd python docs\site\render_images.py <names>`; 480 px JPEGs in `images/`, keep the set under 8 MB). Houdini GUI screenshots go in `images/houdini/`; until one exists, its `![Houdini: ...]` link shows as a placeholder box. |
| Code shortcut (`ponytail:` comment) | Add a row to "Known limitations". Searching the code for `ponytail:` finds them. |

The published page: `python docs/site/build.py docs <out.html>` (one file, pictures embedded; one chapter at a time with a sidebar).

Numbers are GPU milliseconds on a GTX 1080 Ti unless stated.

## Tests

ctest labels: `slow` (over about a minute on a 1080 Ti: vkrtest, aov_sums, motion, lighting, traced_media, screenspace, displacement_graphs, exr, procedural, karma_golden) and `fast` (the rest, about 7 minutes). Run `ctest -j1 -L fast` plus the slow tests of what you changed (`-R`), and everything once before a release. Always `-j1`: the tests share the GPU.

