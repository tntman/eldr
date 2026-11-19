<div align="center">

<p align="center">
  <img src="docs/images/banner.gif" alt="Eldr: A Vulkan Hydra render delegate for OpenUSD, MaterialX and Houdini Solaris" width="100%">
</p>

<h1>Eldr</h1>

<p><b>A GPU raster renderer for OpenUSD and MaterialX. Final VFX frames to EXR in seconds, with Karma as the look it aims for.</b></p>

<p align="center">
  <!-- Maintenance -->
  <img src="https://img.shields.io/badge/maintenance-actively--developed-brightgreen" alt="Maintenance: actively developed">
  &nbsp;&nbsp;
  <!-- License -->
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-Apache_2.0-brightgreen?logo=apache&logoColor=white" alt="License: Apache 2.0"></a>
  &nbsp;&nbsp;
  <!-- Docs -->
  <a href="https://tntman.github.io/eldr/"><img src="https://img.shields.io/github/actions/workflow/status/tntman/eldr/docs.yml?branch=step1-plugin-skeleton&label=Docs&logo=github-actions&logoColor=white" alt="Docs build"></a>
  &nbsp;&nbsp;
  <!-- Platform -->
  <img src="https://img.shields.io/badge/platform-Windows-0078D4?logo=windows&logoColor=white" alt="Platform: Windows">
  &nbsp;&nbsp;
  <!-- Vulkan -->
  <img src="https://img.shields.io/badge/Vulkan-1.3-AC162C?logo=vulkan&logoColor=white" alt="Vulkan 1.3">
  &nbsp;&nbsp;
  <!-- OpenUSD -->
  <img src="https://img.shields.io/badge/OpenUSD-26.08-6A5ACD" alt="OpenUSD 26.08">
  &nbsp;&nbsp;
  <!-- Houdini -->
  <img src="https://img.shields.io/badge/Houdini-22.0-FF4713?logo=houdini&logoColor=white" alt="Houdini 22.0">
  &nbsp;&nbsp;
  <!-- Last Commit -->
  <img src="https://img.shields.io/github/last-commit/tntman/eldr" alt="Last commit">
  &nbsp;&nbsp;
  <!-- Stars -->
  <img src="https://img.shields.io/github/stars/tntman/eldr" alt="GitHub stars">
</p>

<p><a href="https://tntman.github.io/eldr/"><b>Documentation</b></a> · <a href="docs/guide/getting-started.md">Getting started</a> · <a href="docs/settings.md">Settings</a> · <a href="docs/guide/showcase.md">Showcase scenes</a></p>

</div>

<!-- TABLE OF CONTENTS -->
## Table of Contents

- [About](#about)
- [Gallery](#gallery)
- [Features](#features)
- [How It Works](#how-it-works)
- [Architecture](#architecture)
- [Installation](#installation)
- [Usage](#usage)
- [Environment Variables](#environment-variables)
- [Development](#development)
- [License](#license)

<!-- ABOUT -->
## About

Eldr is a Hydra 2.0 render delegate written on plain Vulkan. It plugs into Houdini's Solaris viewport and husk, and into OpenUSD's usdview and usdrecord, and renders the same USD stage Karma does: UsdLux lights, full MaterialX shading (standard_surface, OpenPBR, glTF PBR, pbrlib BSDF graphs, Houdini's Lama and Karma nodes, node graphs compiled into GPU programs) and UsdPreviewSurface, OpenVDB volumes, UsdSkel crowds, cameras with depth of field and motion blur, and AOVs with light groups and Cryptomatte.

It is a raster renderer built for offline VFX, not a game engine. A still frame keeps adding samples (the *film*) until it converges, so shadows, depth of field and motion blur resolve exactly, and a final frame takes seconds instead of minutes. Ray-traced shadows, reflections, refraction, GI and caustics are there when the GPU supports them, and are off by default: the default render never starts ray tracing.

Karma is the reference. Each feature ships a showcase scene with `eldr_default`, `eldr_final` and `karma` render settings, and the test suite compares Eldr against Karma renders of the same scenes.

The display name is **Eldr** everywhere; the plugin id is `HdVkRendererPlugin`, and scripts and husk accept either.

<!-- GALLERY -->
## Gallery

The CC0 Cornell box and one variant per feature, rendered with husk at 512 x 512: Eldr default, Eldr final and Karma side by side. [docs/gallery.md](docs/gallery.md) has each scene's purpose and how it is rendered.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/gallery/times_dark.svg">
  <img alt="Render time per scene for Eldr default, Eldr final and Karma" src="docs/images/gallery/times_light.svg" width="100%">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/gallery/difference_dark.svg">
  <img alt="Mean absolute difference to Karma per scene for Eldr default and Eldr final" src="docs/images/gallery/difference_light.svg" width="100%">
</picture>

| Scene | Eldr default | Eldr final | Karma | Eldr default vs Karma | Eldr final vs Karma |
|---|---:|---:|---:|---|---|
| [base](docs/gallery.md#base) | 3.0 s (0.1 s, 0.9 ms) | 6.1 s (2.9 s, 9.8 ms) | 61.6 s | x0.52, 43% (41%) | x1.07, 7% (4%) |
| [clearcoat](docs/gallery.md#clearcoat) | 3.1 s (0.1 s, 0.9 ms) | 6.6 s (3.3 s, 11.0 ms) | 47.4 s | x0.63, 35% (26%) | x1.04, 5% (4%) |
| [depth_of_field](docs/gallery.md#depth_of_field) | 3.0 s (0.1 s, 0.9 ms) | 6.0 s (2.9 s, 9.5 ms) | 68.4 s | x0.52, 44% (40%) | x1.06, 8% (6%) |
| [displacement](docs/gallery.md#displacement) | 3.1 s (0.1 s, 0.7 ms) | 5.9 s (2.7 s, 9.0 ms) | 48.1 s | x0.63, 37% (28%) | x1.09, 11% (8%) |
| [exposure](docs/gallery.md#exposure) | 3.1 s (0.1 s, 0.7 ms) | 6.0 s (2.9 s, 9.7 ms) | 68.7 s | x0.52, 43% (41%) | x1.07, 7% (4%) |
| [glossy](docs/gallery.md#glossy) | 3.1 s (0.1 s, 1.1 ms) | 6.7 s (3.4 s, 11.5 ms) | 35.4 s | x0.65, 31% (31%) | x1.16, 16% (7%) |
| [height_fog](docs/gallery.md#height_fog) | 3.0 s (0.1 s, 0.8 ms) | 6.0 s (2.9 s, 9.8 ms) | 54.3 s | x0.52, 43% (41%) | x1.07, 7% (4%) |
| [ies](docs/gallery.md#ies) | 2.9 s (0.1 s, 0.7 ms) | 6.0 s (2.9 s, 9.8 ms) | 71.0 s | x0.35, 62% (94%) | x0.98, 9% (9%) |
| [instances](docs/gallery.md#instances) | 3.1 s (0.1 s, 1.1 ms) | 6.7 s (3.4 s, 11.5 ms) | 53.5 s | x0.52, 43% (41%) | x1.08, 8% (5%) |
| [lens_shift](docs/gallery.md#lens_shift) | 2.9 s (0.1 s, 0.6 ms) | 5.6 s (2.5 s, 8.2 ms) | 38.5 s | x0.49, 44% (43%) | x1.07, 6% (4%) |
| [light_linking](docs/gallery.md#light_linking) | 3.0 s (0.1 s, 0.9 ms) | 6.2 s (3.1 s, 10.4 ms) | 71.3 s | x0.60, 39% (48%) | x1.05, 6% (4%) |
| [motion_blur](docs/gallery.md#motion_blur) | 3.1 s (0.1 s, 0.9 ms) | 6.4 s (3.2 s, 10.7 ms) | 56.6 s | x0.52, 43% (41%) | x1.07, 7% (4%) |
| [orthographic](docs/gallery.md#orthographic) | 2.9 s (0.1 s, 0.7 ms) | 5.8 s (2.7 s, 8.9 ms) | 51.0 s | x0.58, 38% (24%) | x1.07, 7% (4%) |
| [post_processing](docs/gallery.md#post_processing) | 2.9 s (0.1 s, 0.7 ms) | 6.0 s (2.9 s, 9.8 ms) | 53.4 s | x0.52, 43% (41%) | x1.07, 7% (4%) |
| [render_region](docs/gallery.md#render_region) | 2.9 s (0.1 s, 0.3 ms) | 4.3 s (1.3 s, 3.9 ms) | 17.9 s | x0.57, 38% (31%) | x1.06, 7% (5%) |
| [subdivision](docs/gallery.md#subdivision) | 3.1 s (0.1 s, 0.7 ms) | 6.3 s (3.1 s, 10.4 ms) | 53.2 s | x0.52, 43% (41%) | x1.07, 7% (4%) |
| [uv_sets](docs/gallery.md#uv_sets) | 3.2 s (0.1 s, 0.9 ms) | 6.1 s (2.8 s, 9.4 ms) | 40.1 s | x0.66, 31% (28%) | x1.10, 9% (6%) |
| [volume](docs/gallery.md#volume) | 3.1 s (0.2 s, 2.2 ms) | 7.6 s (4.3 s, 14.8 ms) | 67.2 s | x0.53, 44% (46%) | x1.01, 7% (6%) |

Times are husk wall clock at 512 x 512, including startup and scene load; in brackets Eldr's time in its render pass and the GPU time of one film sample. *vs Karma* is the mean luminance ratio, then the mean absolute difference as rendered and, in brackets, after scaling the image to Karma's mean luminance (exposure-matched), both as a percentage of Karma's mean luminance.

![base, left to right Eldr default, Eldr final and Karma: Gallery base scene: the CC0 Cornell box (cornell-box.usda, unchanged) with the canonical Cornell camera, its ceiling light as a UsdLux RectLight of the same size and radiance, and render settings shared by hdUE and Karma.](docs/images/gallery/base.jpg)

![clearcoat, left to right Eldr default, Eldr final and Karma: Preview Surface clearcoat: red lacquer (clearcoat 1 over a rough red base, left) next to the same base without a coat (right).](docs/images/gallery/clearcoat.jpg)

![depth_of_field, left to right Eldr default, Eldr final and Karma: Thin-lens depth of field focused on the tall box (USD lens units: tenths of a scene unit).](docs/images/gallery/depth_of_field.jpg)

![displacement, left to right Eldr default, Eldr final and Karma: Preview Surface displacement on a single quad: blurred checker bumps, 0.6 m high, diced and displaced by hdUE.](docs/images/gallery/displacement.jpg)

![exposure, left to right Eldr default, Eldr final and Karma: Camera exposure of +1 stop.](docs/images/gallery/exposure.jpg)

![glossy, left to right Eldr default, Eldr final and Karma: Glossy metal on the tall box and a glossy dielectric ball.](docs/images/gallery/glossy.jpg)

![height_fog, left to right Eldr default, Eldr final and Karma: UE's Exponential Height Fog with Volumetric Fog filling the box, lit by the ceiling light (hdUE only: Karma has no height fog).](docs/images/gallery/height_fog.jpg)

![ies, left to right Eldr default, Eldr final and Karma: The ceiling light replaced by a point light with an IES profile.](docs/images/gallery/ies.jpg)

![instances, left to right Eldr default, Eldr final and Karma: A PointInstancer, Points and a cubic BasisCurves helix, with per-instance and per-curve colours.](docs/images/gallery/instances.jpg)

![lens_shift, left to right Eldr default, Eldr final and Karma: Horizontal and vertical aperture offsets (lens shift).](docs/images/gallery/lens_shift.jpg)

![light_linking, left to right Eldr default, Eldr final and Karma: A warm light linked only to the tall box and a cool one only to the short box; the ceiling light lights everything. Lumen still bounces the linked lights onto every surface: UE’s lighting channels only restrict direct light.](docs/images/gallery/light_linking.jpg)

![motion_blur, left to right Eldr default, Eldr final and Karma: A ball crossing the box during a half-frame shutter.](docs/images/gallery/motion_blur.jpg)

![orthographic, left to right Eldr default, Eldr final and Karma: Orthographic projection with a 6.2 m window.](docs/images/gallery/orthographic.jpg)

![post_processing, left to right Eldr default, Eldr final and Karma: UE's tonemapper (+2.5 stops), bloom, vignette, chromatic aberration, lens flares, film grain and a warm grade on the hdUE beauty; Karma renders the linear image.](docs/images/gallery/post_processing.jpg)

![render_region, left to right Eldr default, Eldr final and Karma: A render region: dataWindowNDC (0.25, 0.15)-(0.75, 0.65) renders only that part of the 512 x 512 frame, as husk tiles do.](docs/images/gallery/render_region.jpg)

![subdivision, left to right Eldr default, Eldr final and Karma: A Catmull-Clark cube (USD default scheme) with a red GeomSubset on two faces, refined to level 3 in hdUE.](docs/images/gallery/subdivision.jpg)

![uv_sets, left to right Eldr default, Eldr final and Karma: Two UV sets: a checker reads st1 (four times st, so 32 x 32 cells) while a left-to-right metallic ramp reads st.](docs/images/gallery/uv_sets.jpg)

![volume, left to right Eldr default, Eldr final and Karma: A fog sphere (OpenVDB, density field) in the middle of the box.](docs/images/gallery/volume.jpg)

A few features, off and on:

| | Off | On |
|---|---|---|
| **Ray-traced and screen-space reflections** | ![SSR off](docs/images/ssr_off.jpg) | ![SSR on](docs/images/ssr_on.jpg) |
| **Volumetric fog** | ![Fog off](docs/images/fog_off.jpg) | ![Fog on](docs/images/fog_on.jpg) |
| **Caustics** | ![Caustics off](docs/images/caustics_off.jpg) | ![Caustics on](docs/images/caustics_on.jpg) |
| **Motion blur** | ![Motion blur off](docs/images/motion_blur_off.jpg) | ![Motion blur on](docs/images/motion_blur_on.jpg) |
| **Displacement** | ![Displacement off](docs/images/displacement_off.jpg) | ![Displacement on](docs/images/displacement_on.jpg) |

Every picture comes from a folder in [`showcase/`](showcase/README.md); the [user guide](docs/guide/getting-started.md) names the folder and settings prim under each one.

<!-- FEATURES -->
## Features

| Area | What you get | Not yet |
|---|---|---|
| **Film and sampling** | Converging film with Sobol and blue-noise sample dimensions, adaptive sampling per 16x16 tile, Karma's per-component quality (`karma:object:diffusequality` and friends), Intel OIDN denoise with albedo and normal guides, tiled rendering for very large frames (`eldr:tileSize`) | Path-traced reference mode (use Karma) |
| **Lights** | Sphere, rect, disk, cylinder, distant and dome lights; textured area lights; IES profiles; light linking and shadow linking; light shapes shown as in Karma (`renderlightgeo`); emissive meshes as lights; HDRI sun extraction; hundreds of lights through screen-space clusters | Light functions, dome portals, IES types A and B |
| **Shadows** | Cascaded sun shadows, local-light atlas, PCSS soft shadows that converge exactly, coloured shadows through glass and volumes, ray-traced shadows (`eldr:rtShadows`) | Virtual shadow maps |
| **Global illumination** | GTAO, screen-space GI, ray-traced GI probes, final gather (`eldr:gi`, `eldr:finalGather`) | Infinite-bounce GI without ray tracing |
| **Reflections and refraction** | Screen-space reflections with back-face depth (measured thickness), planar mirrors, probes, ray-traced reflections with Karma's reflect limit, screen-space and ray-traced refraction with nested media and Beer-Lambert absorption, photon caustics | Refraction through planar water |
| **Materials** | MaterialX `standard_surface`, OpenPBR, glTF PBR and UsdPreviewSurface; pbrlib BSDF graphs and Houdini's Lama layering as Karma renders them; material graphs compiled whole into per-pixel programs; Karma's own nodes (ramps, hex tiling, Voronoi); textures with UDIM and BC7 compression | Burley subsurface (a per-channel Gaussian today), coat normal |
| **Volumes** | OpenVDB and VDB sequences, NanoVDB on the GPU, single and multiple scattering, volume self-shadowing from every light, mutual volume shadows, motion blur from `vel` | Karma's Pyro Shader |
| **Geometry** | Meshes, OpenSubdiv subdivision, displacement, curves (tubes and ribbons), points, point instancers with LOD, UsdSkel crowds with deferred skinning | Vector displacement's tangential part |
| **Camera** | Depth of field, motion blur from time samples and velocities, lens distortion, Karma's lens shaders, exposure from the camera | |
| **Output** | Render products and vars, LPE AOVs, light groups, Cryptomatte, linear beauty for husk, holdouts and mattes | `elementId` AOV |
| **Houdini** | Eldr tab on Render Settings, Render Geometry Settings and light LOPs (same Do Nothing / Set / Block menus as Karma's), viewport stats panel, husk wrapper for crowds | |

The full list, with settings, costs and the [known limitations](docs/features.md#known-limitations), is in [docs/features.md](docs/features.md). [docs/comparison.md](docs/comparison.md) compares Eldr with EEVEE and Unreal feature by feature.

<!-- HOW IT WORKS -->
## How It Works

1. **Sync.** Hydra 2.0 hands Eldr the stage through its scene index. Meshes, curves, instancers, lights, materials, volumes and cameras become GPU buffers; MaterialX graphs compile into material programs; UsdSkel poses once per skeleton and shutter time.
2. **Prepass and lighting.** A depth prepass, culling on the GPU, light clusters, shadow maps and the GI probes run once per sample.
3. **Shading.** One forward pass shades every surface: lights, shadows, dome, SSR, SSGI, AO, then transparency, refraction, subsurface and volumes in their own passes.
4. **Ray tracing (when on).** Shadows, reflections, refraction, GI and caustics trace against a TLAS of the scene, on GPUs with Vulkan ray tracing.
5. **Film.** Each sample jitters the pixel, the lens, the light shapes and the shutter time; the film averages them until the image converges, then denoises if asked.
6. **Output.** The viewport gets the image through GL interop or a readback; husk and usdrecord get linear EXR render products and AOVs.

<!-- ARCHITECTURE -->
## Architecture

```mermaid
flowchart LR
  subgraph Hosts["🎬 Hosts"]
    H[Houdini Solaris / husk]
    U[usdview / usdrecord]
  end
  subgraph Hydra["🧩 hydra/ · Hydra 2.0 delegate"]
    D[Render delegate<br/>render pass, settings]
    P[Prims: mesh, curves, lights,<br/>materials, volumes, cameras]
  end
  subgraph Renderer["⚡ renderer/ · plain Vulkan"]
    S[Scene and GPU buffers]
    R[Passes: prepass, shading,<br/>transparency, volumes, post]
    T[Ray tracing and film]
  end
  H --> D
  U --> D
  D --> P --> S --> R --> T
```

`renderer/` is a Vulkan renderer that never includes a USD header; `hydra/` translates USD into it. Houdini-only code lives in the optional Houdini module, so Eldr itself has no Houdini dependency. Shaders are GLSL in `renderer/shaders/`, compiled to SPIR-V at build time.

<!-- INSTALLATION -->
## Installation

Eldr needs Windows, a GPU with Vulkan 1.3 and a current driver, and either Houdini 22 (22.0.466) or OpenUSD 26.08. A GPU with Vulkan ray tracing is optional: without it Eldr skips the effects marked **[RT]** in the [settings](docs/settings.md).

> [!NOTE]
> There is no binary release yet. Eldr builds from source against your Houdini or USD install; see [docs/houdini.md](docs/houdini.md#build).

1. Build the Houdini plugin (below). The build writes `build-h22/houdini/eldr.json`, a Houdini package.
2. Copy `eldr.json` to `Documents/houdini22.0/packages/`, or set `HOUDINI_PACKAGE_DIR` to `build-h22/houdini`.
3. Start Houdini, go to a LOP network and pick **Eldr** in the viewport's renderer menu or on a Render Settings LOP.
4. Render Settings and light LOPs made before the install get Eldr's tab from **Rebuild Renderer-Specific Parameters**.

<!-- USAGE -->
## Usage

In Solaris, pick Eldr in the viewport. While the camera moves you see a fast image; when it stops, the film refines it to **Film Samples** (64) and stops. From a Houdini command shell, husk renders a frame or a sequence:

```powershell
husk -R Eldr -s /Render/eldr_final -f 1 -n 24 -o render/beauty.$F4.exr showcase\2026-10-30_07_light_shapes\scene.usda
```

Without Houdini, `usdenv.cmd` sets up the plugin paths for USD's own tools:

```powershell
usdenv usdview --renderer Eldr showcase\2026-10-30_07_light_shapes\scene.usda
usdenv usdrecord --renderer Eldr --renderSettingsPrimPath /Render/eldr_final --disableCameraLight scene.usda out.png
```

A final render is a Render Settings prim with the expensive settings ticked one by one; there are no quality presets. Every `eldr:` setting has an on/off for A/B tests.

| Topic | Page |
|---|---|
| First render, install, husk, usdview | [Getting started](docs/guide/getting-started.md) |
| Draft vs final, the Eldr tab | [Rendering in Solaris](docs/guide/solaris.md) |
| Lights, shadows, dome | [Lights](docs/guide/lights.md) |
| MaterialX, textures, Karma nodes | [Materials](docs/guide/materials.md), [MaterialX reference](docs/materialx.md) |
| Ray-traced effects | [Ray tracing](docs/guide/ray-tracing.md) |
| AOVs, light groups, Cryptomatte | [AOVs](docs/guide/aovs.md) |
| Crowds, memory, large frames | [Performance](docs/guide/performance.md) |
| Eldr next to Karma | [Eldr and Karma](docs/guide/karma.md) |
| Every setting: default, effect, cost | [Settings](docs/settings.md) |

<!-- ENVIRONMENT VARIABLES -->
## Environment Variables

| Variable | Description |
|---|---|
| `HDVK_TIMING` | `1` prints startup and per-pass GPU times (per tile for tiled renders); `2` writes a CSV row per frame to `HDVK_TIMING_CSV` |
| `HDVK_PIPELINE_CACHE_DIR` | Moves the shader pipeline cache and the compressed-texture cache (default `%LOCALAPPDATA%\Eldr`) |
| `HDVK_NO_RAY_TRACING` | `1` behaves as if the GPU had no ray tracing |
| `HDVK_RT_EARLY_COMPILE` | `0` compiles ray tracing pipelines on first trace instead of on background threads |
| `HDVK_NO_STATS` | `1` reports only the renderer name to the host's stats (for comparison) |
| `HD_ENABLE_DEFERRED_SKINNING` | `1` lets Eldr pose UsdSkel crowds on the GPU (set by the husk wrapper `$ELDR_HUSK`) |
| `USD_ROOT`, `USD_VENV`, `HDVK_BUILD` | Read by `usdenv.cmd` to find USD, its Python and the Eldr build |

<!-- DEVELOPMENT -->
## Development

Building needs Visual Studio 2022 (x64), CMake with Ninja, the Vulkan SDK (glslc), and Houdini 22.0.466 or OpenUSD 26.08 built with OpenVDB. Intel OIDN 2.x is optional (for `eldr:denoise`); `-DHDVK_SCCACHE=ON` uses sccache.

```powershell
# Houdini build (Solaris, husk)
cmake -S . -B build-h22 -G Ninja -DCMAKE_BUILD_TYPE=Release -DHDVK_HOUDINI=ON "-DHOUDINI_ROOT=C:/Program Files/Side Effects Software/Houdini 22.0.466"
cmake --build build-h22

# OpenUSD build (usdview, usdrecord, tests)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Tests share the GPU: always one at a time
ctest --test-dir build -j1 -L fast
```

The test suite has about 55 ctests: renderer unit checks (`vkrtest`), Hydra and USD round trips, Karma golden references, a reflection and refraction scenario suite (`tests/scenes/scenarios`) and the docs check that every setting is documented. `tests/compare_renderers.py` renders any scene with Eldr, Karma and Blender EEVEE side by side ([docs/testing.md](docs/testing.md)). The documentation site builds from `docs/` with `docs/site/build.py` and publishes to GitHub Pages on every push.

<!-- LICENSE -->
## License

Eldr is licensed under the [Apache License 2.0](LICENSE).

`third_party/nanovdb` holds NanoVDB's `PNanoVDB.h` from OpenVDB (MPL-2.0, unmodified). OpenUSD, Houdini, Vulkan, Intel OIDN and MaterialX keep their own licences.

Banner rendered in Intel Sponza (Frank Meinl, Anton Kaplanyan, Intel Sample Library, 2022) with the add-on packages by the Sponza Add-on Package Crew; CC BY 4.0. Rendered with Eldr by `docs/site/banner.py`.
