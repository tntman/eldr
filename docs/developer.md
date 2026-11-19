# Developer guide

How the source is laid out, how to build and test it, and the rules a change follows. [How Eldr works](features.md) describes the renderer itself, feature by feature.

## Source layout

| Folder | What it holds |
|---|---|
| `renderer/` | The Vulkan renderer: scene buffers, passes (prepass, shading, transparency, volumes, post), ray tracing, film, denoise. It never includes a USD header |
| `renderer/shaders/` | GLSL, compiled to SPIR-V at build time with glslc |
| `hydra/` | The Hydra 2.0 render delegate: prims (mesh, curves, lights, materials, volumes, cameras), render pass, render products, scene index plugins. `renderSettings.def` lists every render setting |
| `houdini/` | The optional Houdini module: build glue (`houdini.cmake`), the package, the Solaris tabs (`gen_ui.py`), the render stats panel, Houdini's OpenVDB reader |
| `usdview/` | usdview's **Eldr** menu: render settings window, render statistics, tooltips |
| `tests/` | The ctest suite, test scenes (`tests/scenes`), Karma references and comparison tools |
| `showcase/` | One folder per feature: `scene.usda`, `scene.hip` and a README |
| `houdini-tests/` | One `.hip` per Houdini checklist item |
| `docs/` | This documentation; `docs/site/build.py` builds the page |
| `third_party/` | NanoVDB's `PNanoVDB.h` |

Eldr has no Houdini dependency: Houdini-only code lives in `houdini/` and builds only with `-DHDVK_HOUDINI=ON`.

## Build

Two builds from the same source: one against Houdini's USD (Solaris, husk), one against OpenUSD 26.08 (usdview, usdrecord, the tests). Both need Visual Studio 2022 (x64), CMake with Ninja and the Vulkan SDK; Intel OIDN 2.x is optional (Denoise).

```
cmake -S . -B build-h22 -G Ninja -DCMAKE_BUILD_TYPE=Release -DHDVK_HOUDINI=ON "-DHOUDINI_ROOT=C:/Program Files/Side Effects Software/Houdini 22.0.466"
cmake --build build-h22

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The Houdini build's differences (targets, EXR products, volumes) are on the [Houdini page](houdini.md#build).

## Test

```
ctest --test-dir build -j1 -L fast
```

The tests share the GPU, so always run them one at a time (`-j1`). `fast` takes about 7 minutes; the `slow` label holds the long renders. [Testing](testing.md) covers the Karma golden references and the side-by-side comparisons with Karma and Blender EEVEE.

## Rules for a change

| Change | Rule |
|---|---|
| A render setting | One line in `hydra/renderSettings.def`, in its group; read it in `hydra/renderPass.cpp`; a row in `docs/settings.md` at the same place. The `docs` test checks the rows |
| An image-changing feature | Gets an on/off setting, for A/B tests and speed |
| Ray tracing | The default settings never start ray tracing (the `no_implicit_rt` test) |
| USD attributes | The USD standard name first, then Karma's `karma:` name, then an `eldr:` twin. Eldr's wins when set |
| A shortcut with a known limit | A `ponytail:` comment in the code, and a row in [Known limitations](features.md#known-limitations) |
| A user-visible feature | A showcase scene, a section in the matching user-guide chapter, and its row in [the comparison](comparison.md) |

## Writing the docs

[Writing the docs](README.md) says which page each kind of change updates and how the pictures are made. The voice: for a new artist, present tense, brief, no history.
