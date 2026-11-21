<div align="center">

<p align="center">
  <img src="docs/images/banner.webp" alt="Eldr: A Vulkan Hydra render delegate for OpenUSD, MaterialX and Houdini Solaris" width="100%">
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
- [Eldr vs EEVEE and Unreal](#eldr-vs-eevee-and-unreal)
- [How It Works](#how-it-works)
- [Architecture](#architecture)
- [Installation](#installation)
- [Usage](#usage)
- [Environment Variables](#environment-variables)
- [Development](#development)
- [Credits](#credits)
- [License](#license)

<!-- ABOUT -->
## About

Eldr is a Hydra 2.0 render delegate written on plain Vulkan. It plugs into Houdini's Solaris viewport and husk, and into OpenUSD's usdview and usdrecord, and renders the same USD stage Karma does: UsdLux lights, full MaterialX shading (standard_surface, OpenPBR, glTF PBR, pbrlib BSDF graphs, Houdini's Lama and Karma nodes, node graphs compiled into GPU programs) and UsdPreviewSurface, OpenVDB volumes, UsdSkel crowds, cameras with depth of field and motion blur, and AOVs with light groups and Cryptomatte.

It is a raster renderer built for offline VFX, not a game engine. A still frame keeps adding samples (the *film*) until it converges, so shadows, depth of field and motion blur resolve exactly, and a final frame takes seconds instead of minutes. Ray-traced shadows, reflections, refraction, GI and caustics are there when the GPU supports them, and are off by default: the default render never starts ray tracing.

Karma is the reference. Each feature ships a showcase scene with `eldr_default`, `eldr_final` and `karma` render settings, and the test suite compares Eldr against Karma renders of the same scenes.

The display name is **Eldr** everywhere; the plugin id is `HdVkRendererPlugin`, and scripts and husk accept either.

<!-- GALLERY -->
## Gallery

*Hardware: AMD Ryzen 9 3950X (16 cores, 32 threads), 48 GB RAM, NVIDIA GeForce GTX 1080 Ti 11 GB (driver 582.78; no RT cores, so Eldr's ray tracing runs through its compute fallback), Windows 11 Pro, Houdini 22.0.466. Karma CPU runs on the Ryzen, Karma XPU on the GTX 1080 Ti (with the CPU).*

Every scene renders four ways with husk: ● **Eldr default**, every effect that works without ray tracing (screen-space GI, reflections and refraction, planar mirrors, ambient occlusion, subsurface, soft shadow maps, denoising, 256 film samples: `tests/scenes/cornell/eldr_default.usda`); ◆ **Eldr final**, ray traced (reflections, refraction, shadows, GI, final gather, caustics, denoising, 256 film samples: `tests/scenes/cornell/eldr_final.usda`); ▲ **Karma XPU** and ■ **Karma CPU** with the scene's Karma settings. Karma CPU is the brightness reference.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/gallery/times_dark.svg">
  <img alt="Render time per scene for each renderer" src="docs/images/gallery/times_light.svg" width="100%">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/gallery/brightness_dark.svg">
  <img alt="Mean brightness relative to Karma CPU per scene" src="docs/images/gallery/brightness_light.svg" width="100%">
</picture>

| Scene | Eldr default | Eldr final | Karma XPU | Karma CPU | Brightness vs Karma CPU (default / final / XPU) |
|---|---:|---:|---:|---:|---|
| [Global illumination](docs/gallery.md#global-illumination) | 947 ms | 3.1 s | 25.1 s | 47.2 s | 0.53× / 1.02× / 1.02× |
| [Clearcoat](docs/gallery.md#clearcoat) | 3.9 s | 3.4 s | 11.0 s | 37.0 s | 0.64× / 1.01× / 1.02× |
| [Depth of field](docs/gallery.md#depth-of-field) | 877 ms | 2.9 s | 15.4 s | 49.2 s | 0.53× / 1.01× / 1.02× |
| [Displacement](docs/gallery.md#displacement) | 4.7 s | 10.2 s | 11.3 s | 36.8 s | 0.63× / 1.02× / 1.02× |
| [Emissive surface lighting](docs/gallery.md#emissive-surface-lighting) | 2.5 s | 6.3 s | 35.4 s | 110.8 s | 0.78× / 1.00× / 1.02× |
| [Camera exposure](docs/gallery.md#camera-exposure) | 876 ms | 2.9 s | 17.3 s | 57.3 s | 0.53× / 1.02× / 1.02× |
| [Glass and caustics](docs/gallery.md#glass-and-caustics) | 4.0 s | 8.4 s | 36.0 s | 55.3 s | 0.53× / 1.03× / 1.02× |
| [Glossy materials](docs/gallery.md#glossy-materials) | 2.9 s | 3.4 s | 9.0 s | 29.7 s | 0.66× / 1.02× / 1.02× |
| [Hair](docs/gallery.md#hair) | 3.4 s | 8.0 s | 21.2 s | 49.4 s | 0.54× / 1.03× / 1.02× |
| [Unreal height fog settings](docs/gallery.md#unreal-height-fog-settings) | 932 ms | 3.0 s | 14.5 s | 48.1 s | 0.53× / 1.02× / 1.02× |
| [Holdout](docs/gallery.md#holdout) | 2.4 s | 6.1 s | 56.8 s | 48.1 s | 0.53× / 1.03× / 1.02× |
| [IES profile](docs/gallery.md#ies-profile) | 878 ms | 2.9 s | 19.7 s | 66.4 s | 0.35× / 0.94× / 1.07× |
| [Instances, points, curves](docs/gallery.md#instances-points-curves) | 1.1 s | 3.4 s | 17.1 s | 50.0 s | 0.53× / 1.02× / 1.02× |
| [Lens shift](docs/gallery.md#lens-shift) | 825 ms | 2.5 s | 10.6 s | 34.2 s | 0.49× / 1.02× / 1.02× |
| [Light groups](docs/gallery.md#light-groups) | 2.7 s | 6.7 s | 28.8 s | 68.2 s | 0.61× / 1.02× / 1.02× |
| [Light linking](docs/gallery.md#light-linking) | 936 ms | 3.1 s | 23.7 s | 63.2 s | 0.60× / 1.02× / 1.02× |
| [Textured area light](docs/gallery.md#textured-area-light) | 1.2 s | 3.9 s | 20.8 s | 62.7 s | 0.55× / 1.03× / 1.03× |
| [Sheen, thin film, anisotropy](docs/gallery.md#sheen-thin-film-anisotropy) | 1.0 s | 3.4 s | 15.7 s | 51.1 s | 0.53× / 1.02× / 1.02× |
| [Motion blur](docs/gallery.md#motion-blur) | 2.7 s | 6.3 s | 27.4 s | 50.1 s | 0.53× / 1.02× / 1.02× |
| [Orthographic camera](docs/gallery.md#orthographic-camera) | 836 ms | 2.7 s | 14.1 s | 45.7 s | 0.58× / 1.03× / 1.01× |
| [Unreal post-processing settings](docs/gallery.md#unreal-post-processing-settings) | 860 ms | 2.9 s | 14.8 s | 48.1 s | 0.53× / 1.02× / 1.02× |
| [Render region](docs/gallery.md#render-region) | 282 ms | 1.4 s | 3.9 s | 13.3 s | 0.57× / 1.01× / 1.01× |
| [Subdivision surfaces](docs/gallery.md#subdivision-surfaces) | 837 ms | 3.1 s | 14.7 s | 48.8 s | 0.53× / 1.02× / 1.02× |
| [Subsurface scattering](docs/gallery.md#subsurface-scattering) | 2.7 s | 6.9 s | 21.9 s | 56.9 s | 0.51× / 1.01× / 1.02× |
| [UV sets](docs/gallery.md#uv-sets) | 3.3 s | 6.4 s | 16.6 s | 33.0 s | 0.68× / 1.06× / 1.02× |
| [Volumes (OpenVDB)](docs/gallery.md#volumes-openvdb) | 3.1 s | 7.6 s | 26.0 s | 59.8 s | 0.54× / 0.98× / 1.02× |
| [Material lobes](docs/gallery.md#material-lobes) | 8.0 s | 12.1 s | 2.2 s | 7.7 s | 0.96× / 0.98× / 0.98× |
| [Skin, jade and leaves](docs/gallery.md#skin-jade-and-leaves) | 3.8 s | 7.6 s | 18.5 s | 30.5 s | 0.95× / 0.94× / 1.01× |
| [Caustics](docs/gallery.md#caustics) | 5.4 s | 11.6 s | 4.8 s | 14.8 s | 0.99× / 1.07× / 1.01× |
| [Reflections](docs/gallery.md#reflections) | 3.9 s | 4.6 s | 470 ms | 4.5 s | 1.00× / 1.00× / 1.00× |
| [Curves, points and hair](docs/gallery.md#curves-points-and-hair) | 4.3 s | 6.5 s | 460 ms | 1.4 s | 1.02× / 1.02× / 1.00× |
| [Light shapes](docs/gallery.md#light-shapes) | 4.7 s | 6.3 s | 1.9 s | 6.4 s | 1.12× / 0.99× / 1.01× |
| [Many lights](docs/gallery.md#many-lights) | 16.8 s | 24.7 s | 38.8 s | 100.6 s | 0.88× / 0.94× / 1.02× |
| [Volume multiple scattering](docs/gallery.md#volume-multiple-scattering) | 7.5 s | 11.2 s | 66.1 s | 432.2 s | 0.63× / 1.02× / 1.00× |
| [Displacement shaders](docs/gallery.md#displacement-shaders) | 4.4 s | 6.9 s | 9.9 s | 5.4 s | 1.01× / 0.99× / 0.99× |

[docs/gallery.md](docs/gallery.md) has the methodology.

### Cornell box: one feature per scene

The CC0 Cornell box (`tests/scenes/cornell`) plus one small variant layer, rendered at 512 × 512.

#### Global illumination

The CC0 Cornell box with its measured camera and a RectLight matching the ceiling light.

![Global illumination: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/base.jpg)

#### Clearcoat

Preview Surface clearcoat: red lacquer (clearcoat 1 over a rough red base, left) next to the same base without a coat (right).

![Clearcoat: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/clearcoat.jpg)

#### Depth of field

Thin-lens depth of field focused on the tall box (USD lens units: tenths of a scene unit).

![Depth of field: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/depth_of_field.jpg)

#### Displacement

Preview Surface displacement on a single quad: blurred checker bumps, 0.6 m high.

![Displacement: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/displacement.jpg)

#### Emissive surface lighting

The ceiling light is the asset's emissive quad instead of a RectLight: an emissive surface lights the room.

![Emissive surface lighting: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/emissive.jpg)

#### Camera exposure

Camera exposure of +1 stop.

![Camera exposure: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/exposure.jpg)

#### Glass and caustics

A clear glass ball under the ceiling light: refraction, and the caustic it focuses on the floor.

![Glass and caustics: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/glass.jpg)

#### Glossy materials

Glossy metal on the tall box and a glossy dielectric ball.

![Glossy materials: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/glossy.jpg)

#### Hair

A patch of 900 hair curves on the short box, brown, shaded as hair.

![Hair: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/hair.jpg)

#### Unreal height fog settings

Unreal's height fog settings (`unreal:`) on the base scene; Eldr and Karma ignore them.

![Unreal height fog settings: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/height_fog.jpg)

#### Holdout

A holdout ball (Karma's Matte mode): cut out of the image as black with alpha 0, its shadow still on the floor.

![Holdout: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/holdout.jpg)

#### IES profile

The ceiling light replaced by a point light with an IES profile.

![IES profile: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/ies.jpg)

#### Instances, points, curves

A PointInstancer, Points and a cubic BasisCurves helix, with per-instance and per-curve colours.

![Instances, points, curves: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/instances.jpg)

#### Lens shift

Horizontal and vertical aperture offsets (lens shift).

![Lens shift: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/lens_shift.jpg)

#### Light groups

Two light groups, the ceiling light and a warm lamp, written as their own AOVs; the gallery shows the beauty.

![Light groups: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/light_groups.jpg)

#### Light linking

A warm light linked only to the tall box and a cool one only to the short box; the ceiling light lights everything.

![Light linking: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/light_linking.jpg)

#### Textured area light

The ceiling light shows a four-colour texture, which tints the walls, the floor and a chrome ball.

![Textured area light: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/light_texture.jpg)

#### Sheen, thin film, anisotropy

Three material lobes on balls along the front: velvet sheen, thin-film iridescence and brushed anisotropic metal.

![Sheen, thin film, anisotropy: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/lobes.jpg)

#### Motion blur

A ball crossing the box during a half-frame shutter.

![Motion blur: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/motion_blur.jpg)

#### Orthographic camera

Orthographic projection with a 6.2 m window.

![Orthographic camera: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/orthographic.jpg)

#### Unreal post-processing settings

Unreal's post-processing settings (`unreal:`) on the base scene; Eldr and Karma ignore them and render the linear image.

![Unreal post-processing settings: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/post_processing.jpg)

#### Render region

A render region: dataWindowNDC (0.25, 0.15)-(0.75, 0.65) renders only that part of the frame.

![Render region: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/render_region.jpg)

#### Subdivision surfaces

A Catmull-Clark cube (USD's default scheme) with a red GeomSubset on two faces.

![Subdivision surfaces: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/subdivision.jpg)

#### Subsurface scattering

Subsurface scattering: a jade ball on the floor and a wax ball on the short box.

![Subsurface scattering: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/subsurface.jpg)

#### UV sets

Two UV sets: a checker reads st1 (four times st, so 32 x 32 cells) while a left-to-right metallic ramp reads st.

![UV sets: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/uv_sets.jpg)

#### Volumes (OpenVDB)

A fog sphere (OpenVDB, density field) in the middle of the box.

![Volumes (OpenVDB): Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/volume.jpg)

### Feature scenes

Showcase scenes (`showcase/`) for major features, rendered at their own resolution.

![Material lobes: clearcoat, sheen, fuzz, thin film, anisotropic highlights, hair and toon shading](docs/images/hero.jpg)

*Clearcoat, sheen, fuzz, thin film, anisotropic highlights, hair and toon shading, rendered by Eldr (`showcase/2026-10-30_22_material_lobes`).*

#### Material lobes

Clearcoat, sheen, fuzz, thin film, anisotropic highlights, hair and toon shading. (`showcase/2026-10-30_22_material_lobes`)

![Material lobes: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/material_lobes.jpg)

#### Skin, jade and leaves

Light scatters under the surface of skin, jade and leaves. (`showcase/2026-10-30_13_subsurface`)

![Skin, jade and leaves: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/subsurface.jpg)

#### Caustics

Light focused through glass lands as a bright patch on other surfaces. (`showcase/2026-10-29_13_caustics`)

![Caustics: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/caustics.jpg)

#### Reflections

A ball on a mirror-like floor reflects as a ball, with its highlights, and reflections reflect again up to a limit. (`showcase/2026-11-04_01_reflections`)

![Reflections: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/reflections.jpg)

#### Curves, points and hair

Every curve type as tubes or ribbons, points as spheres or discs, and hair shading. (`showcase/2026-10-30_19_curves_points`)

![Curves, points and hair: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/curves_points.jpg)

#### Light shapes

Sun, sphere, rect, disk and tube lights give highlights of their own shape, and lights set to show their shape appear to the camera. (`showcase/2026-10-30_07_light_shapes`)

![Light shapes: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/light_shapes.jpg)

#### Many lights

Hundreds of small lights are shaded cheaply: each pixel is lit only by the lights that reach it. (`showcase/2026-10-30_06_many_lights`)

![Many lights: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/many_lights.jpg)

#### Volume multiple scattering

Light bounces more than once inside smoke, set with Karma's Volume Limit. (`showcase/2026-10-29_26_volume_multiple_scattering`)

![Volume multiple scattering: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/volume_scattering.jpg)

#### Displacement shaders

Displacement shaders move the surface and cast shadows. (`showcase/2026-10-30_18_displacement`)

![Displacement shaders: Eldr default, Eldr final, Karma XPU, Karma CPU](docs/images/gallery/features/displacement.jpg)

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

<!-- COMPARISON -->
## Eldr vs EEVEE and Unreal
✅ production-ready · 🟡 partial (see note) · ❌ missing


EEVEE is Blender 5.2; its and Unreal's columns come from their docs and are indicative. *(interactive)* marks rows that matter mostly for games or live work.

<details open>
<summary><b>Rendering core</b></summary>

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Native USD / Hydra render delegate | ✅ | ❌ | 🟡 | Unreal imports USD stages; Blender only has a separate Hydra Storm add-on. |
| Converging final frames (sample accumulation) | ✅ | ✅ | ✅ | Eldr: film. Unreal: Movie Render Queue. |
| Per-effect low-discrepancy sampling | ✅ | ✅ | ✅ | Eldr: Sobol and blue-noise sample dimensions. |
| Per-component sample quality (Karma's diffuse/reflect/light quality) | ✅ | 🟡 | ✅ | Eldr: `eldr:*Quality` or Karma's names, global (from the render settings), not per object. EEVEE: shadow rays and steps. Unreal: Lumen final gather and reflection quality. |
| Adaptive sampling | ✅ | ❌ | ❌ | Eldr stops when 99.9% of pixels converged; 16x16 tiles that stopped skip shading and per-pixel rays (not under refraction, subsurface or post depth of field). Not with motion blur time samples. |
| Denoiser for the final image | ✅ | ✅ (OIDN, OptiX) | 🟡 | Eldr: Intel Open Image Denoise (OIDN) on the CPU (`eldr:denoise`), albedo and normal guides, once per converged film. Unreal: its path tracer only. |
| Path-traced reference mode | ❌ | ✅ (Cycles) | ✅ | Eldr: use Karma in Houdini for ground truth. |
| Background shader compile and pipeline cache | ✅ | ✅ | ✅ | |

</details>

<details>
<summary><b>Lights and shadows</b></summary>

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Sphere, rect, disk, cylinder, distant and dome lights | ✅ | ✅ | ✅ | |
| Textured area lights | ✅ | ❌ | ✅ | Eldr: rect and disk lights, oriented per UsdLux. |
| IES profiles | ✅ | ❌ | ✅ | Eldr: UsdLux `shaping:ies:*`, type C. |
| Light linking | ✅ | ✅ | 🟡 | Unreal has lighting channels. Eldr: UsdLux light and shadow links, 32 collections, not on domes. |
| Light functions / gobos | ❌ | ❌ | ✅ | |
| Light shapes visible to the camera | ✅ | ✅ | ✅ | Eldr: a light shows its shape when `renderlightgeo` is on, as in Karma; `eldr:lightShapesVisible` is the master switch. |
| Soft shadows that converge exactly | ✅ | ✅ | ✅ | Eldr jitters the light shape per film sample (`eldr:shadowJitter`). |
| Virtual shadow maps | ❌ | ✅ | ✅ | Eldr uses cascades and an atlas; ray-traced shadows cover final frames. |
| Ray-traced shadows | ✅ | ❌ | ✅ | Eldr: `eldr:rtShadows` or `eldr:rtShadowsAuto`; sun plus up to 15 lights (`eldr:rtShadowLights`), one ray per light and film sample, exact penumbra. |
| Ray-traced shadows through glass and volumes | ✅ | ❌ | 🟡 | Eldr: tinted by glass, marched through volumes (`eldr:rtTranslucentShadows`). |
| Coloured shadows through glass | ✅ | 🟡 | ✅ | Eldr: shadow maps and ray-traced shadows alike. |
| Volumes casting shadows | ✅ | ✅ | ✅ | Eldr: sun and local lights. |
| Volumes shadowing each other | ✅ | ✅ | 🟡 | Eldr: sun, local lights and the dome's light from its pole (`eldr:volumeMutualShadows`). EEVEE: one shared froxel grid. |
| Sun extracted from an HDRI dome | ✅ | ✅ | ❌ | |

</details>

<details>
<summary><b>Global illumination and reflections</b></summary>

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Dynamic probe GI | ✅ | 🟡 | ✅ | Eldr: ray-traced light probes (DDGI) that move off walls, plus a per-pixel final gather (`eldr:finalGather`). EEVEE: baked irradiance volumes. Unreal: Lumen. |
| Screen-space GI | ✅ | ✅ | ✅ | Eldr: horizon or visibility-bitmask screen-space GI (SSGI). |
| GI at Lumen quality (surface cache, many bounces) | ❌ | ❌ | ✅ | |
| Diffuse bounce limit like Karma's | ✅ | ❌ | 🟡 | Unreal: its path tracer only. Eldr's GI follows `eldr:diffuseLimit`, unset Karma's `karma:object:diffuselimit` (default 1). Cornell box, Eldr final over Karma's mean: x1.00 at limit 1, x1.00 at 2, x1.02 at 8 (docs/testing.md). |
| Screen-space reflections with a denoiser | ✅ | ✅ | ✅ | |
| Ray-traced reflections | ✅ | ❌ | ✅ | |
| Authored reflection probes | ✅ | ✅ | ✅ | Eldr: up to 8, from flagged meshes. |
| Planar mirrors | 🟡 | ✅ | ✅ | Eldr: up to 4, blurred by roughness (`eldr:planarBlur`), no contact hardening. EEVEE's planars are glossy too. |

</details>

<details>
<summary><b>Transparency, volumes and atmosphere</b></summary>

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Order-independent transparency | ✅ | ❌ | 🟡 | |
| Refraction through several layers, back faces, TIR | ✅ | 🟡 | ✅ | Screen space by default; traced with `eldr:rtRefraction`. |
| Ray-traced refraction (off-screen content) | ✅ | ❌ | 🟡 | Eldr: `eldr:rtRefraction`; glass behind glass, materials evaluated at every hit. |
| Ray-traced refraction through displaced glass | ✅ | ❌ | ❌ | Eldr: rippled water bends the rays (`eldr:rtRefractionDisplacement`). |
| Holdouts and mattes | ✅ | ✅ | ✅ | |
| OpenVDB / NanoVDB volumes | ✅ | ✅ | ✅ | Eldr: `UsdVolVolume` with MaterialX or `kma_volume` materials, coloured extinction. Not Karma's Pyro Shader graph. |
| Houdini SOP volumes (no files) | ✅ | ❌ | ❌ | Eldr: VDB and Houdini volumes from a SOP Import LOP, in the Houdini build. |
| Froxel fog (grid aligned to the camera view) | ✅ | ✅ | ✅ | |
| Physical sky and atmosphere | ❌ | 🟡 | ✅ | EEVEE has a sky texture. |
| Volumetric clouds | ❌ | ❌ | ✅ | |

</details>

<details>
<summary><b>Materials and geometry</b></summary>

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| MaterialX, OpenPBR, standard_surface | ✅ | ❌ | 🟡 | Blender uses its own node system; Unreal imports MaterialX into Substrate. |
| GGX multiple-scattering energy compensation | ✅ | ✅ | ✅ | Eldr: `eldr:multiScatter` (on) for standard_surface and OpenPBR; UsdPreviewSurface is single-scatter, as Karma's. |
| Textured lobe inputs (coat, film, transmission, SSS) | ✅ | ✅ | ✅ | |
| Subsurface scattering | ✅ | ✅ | ✅ | |
| Hair rendered as strands | 🟡 | ✅ | ✅ | Eldr: tubes or camera-facing ribbons with a hair BSDF. |
| Displacement, including from graphs | ✅ | ✅ | ✅ | Unreal: Nanite tessellation. |
| Displacement seen by ray tracing, incl. per-instance graphs | ✅ | ❌ | 🟡 | Eldr: acceleration structures built from displaced vertices; graphs that differ per instance get one per instance (the 256 largest on screen). |
| Subdivision surfaces | ✅ | ✅ | 🟡 | Eldr: uniform levels per mesh, from the host, the mesh, or its size on screen (`eldr:screenDicing`) (Karma's dicing quality); no per-face adaptive dicing. |
| Per-object ray visibility (camera, shadow, reflections) | 🟡 | ✅ | ✅ | Eldr: Karma's render visibility; reflections and GI only when ray traced, screen-space effects see what the camera sees. |
| LOD with crossfade | ✅ | ❌ | ✅ | Unreal: Nanite. |
| Virtual geometry (Nanite-style) | ❌ | ❌ | ✅ | *(interactive)* |
| Texture streaming | ❌ | ❌ | ✅ | Needed for very large assets. |
| Ptex | ❌ | ❌ | ❌ | Not planned. |

</details>

<details>
<summary><b>Camera and motion</b></summary>

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Depth of field from lens samples | ✅ | ✅ | ✅ | Eldr adds a residual post blur at low sample counts. |
| Motion blur from shutter time samples | ✅ | ✅ | ✅ | Eldr: smooth blur between time samples (`eldr:motionBlurResidual`); not adaptive. Traced and planar reflections blur too, probes don't. |
| Deformation blur (point caches, skinning, blend shapes) | ✅ | ✅ | 🟡 | Eldr: skinned joints and blend shapes blur in time samples, from the shutter's samples in a single frame (sampled motion). |
| Motion blur on a single frame | ✅ | ✅ | ✅ | Eldr: Hydra transform and point samples. |
| OpenCV lens distortion | ✅ | ❌ | 🟡 | |
| Physical camera exposure (EV, ISO, shutter, f-stop) | ✅ | 🟡 | ✅ | Eldr: UsdGeomCamera's exposure attributes, as Karma (beauty and light AOVs, colour limit after exposure; Karma's ratios within 0.1% without the limit). EEVEE: one exposure value in colour management. |
| Upscaling while interactive | 🟡 | ❌ | ✅ | Eldr has its own temporal anti-aliasing (TAA) upscaler; Unreal has TSR and DLSS. *(interactive)* |

</details>

<details>
<summary><b>Output and pipeline</b></summary>

| Feature | Eldr | EEVEE | Unreal 5 | Notes |
|---|---|---|---|---|
| Multi-layer EXR with AOVs | ✅ | ✅ | ✅ | |
| Light-split AOVs that sum to the beauty | ✅ | ✅ | 🟡 | Eldr: Karma's LPE names. |
| Per-AOV sample filter (min, max, closest, nearest, box, Gauss) | ✅ | ❌ | ❌ | Eldr: Karma's Render Var "Sample Blending" or `driver:parameters:aov:eldr:filter`, film only. |
| Cryptomatte with soft edges (3 layers, 6 ranks) | ✅ | 🟡 | ❌ | Eldr: coverage from the film's samples, Karma's `<var>00..02` layout. EEVEE: hard edges. |
| Cryptomatte | ✅ | ✅ | ✅ | |
| UsdRenderSettings and render products | ✅ | ❌ | ❌ | Eldr honours resolution, pixel aspect and data window. |
| Houdini Solaris | ✅ | ❌ | ❌ | Eldr: Houdini 22 build renders in the Solaris viewport, hython and husk (husk needs Indie or above), with its own Render Settings and Render Geometry Settings tabs. |
| usdview integration | ✅ | ❌ | ❌ | |

</details>

The same table, with notes kept up to date, is in [docs/comparison.md](docs/comparison.md).

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
| A scene from empty to a final EXR, step by step | [Tutorial](docs/guide/tutorial.md) |
| Draft vs final, the Eldr tab | [Rendering in Solaris](docs/guide/solaris.md) |
| Lights, shadows, dome | [Lights](docs/guide/lights.md) |
| MaterialX, textures, Karma nodes | [Materials](docs/guide/materials.md), [MaterialX reference](docs/materialx.md) |
| Ray-traced effects | [Ray tracing](docs/guide/ray-tracing.md) |
| AOVs, light groups, Cryptomatte | [AOVs](docs/guide/aovs.md) |
| Crowds, memory, large frames | [Performance](docs/guide/performance.md) |
| Eldr next to Karma | [Eldr and Karma](docs/guide/karma.md) |
| Every setting: default, effect, cost | [Settings](docs/settings.md) |
| Something looks wrong | [Troubleshooting](docs/guide/troubleshooting.md), [FAQ](docs/guide/faq.md) |

<!-- ENVIRONMENT VARIABLES -->
## Environment Variables

| Variable | Description |
|---|---|
| `HDVK_TIMING` | `1` prints startup and per-pass GPU times (per tile for tiled renders); `2` writes a CSV row per frame to `HDVK_TIMING_CSV` |
| `HDVK_PIPELINE_CACHE_DIR` | Moves the shader pipeline cache and the compressed-texture cache (default `%LOCALAPPDATA%\Eldr`) |
| `HDVK_NO_RAY_TRACING` | `1` behaves as if the GPU had no ray tracing |
| `HDVK_RT_EARLY_COMPILE` | `0` compiles ray tracing pipelines on first trace instead of on background threads |
| `HDVK_OIDN_DEVICE` | Where Denoise runs: `cpu` (default), `cuda` or `default` |
| `HDVK_DEBUG_AOVS` | `1` prints the AOVs the host binds, and every 60 frames what restarted the film |
| `HDVK_NO_STATS` | `1` reports only the renderer name to the host's stats (for comparison) |
| `HD_ENABLE_DEFERRED_SKINNING` | `1` lets Eldr pose UsdSkel crowds on the GPU (set by the husk wrapper `$ELDR_HUSK`) |
| `USD_ROOT`, `USD_VENV`, `HDVK_BUILD` | Read by `usdenv.cmd` to find USD, its Python and the Eldr build |

The full list, with what the Houdini package sets and the debug variables, is in [docs/environment.md](docs/environment.md).

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

The test suite has about 55 ctests: renderer unit checks (`vkrtest`), Hydra and USD round trips, Karma golden references, a reflection and refraction scenario suite (`tests/scenes/scenarios`) and the docs check that every setting is documented. `tests/compare_renderers.py` renders any scene with Eldr, Karma and Blender EEVEE side by side ([docs/testing.md](docs/testing.md)). The documentation site builds from `docs/` with `docs/site/build.py` and publishes to GitHub Pages on every push. The [developer guide](docs/developer.md) has the source layout and the rules for a change.

<!-- CREDITS -->
## Credits

Eldr builds on [OpenUSD](https://openusd.org) and its Hydra framework, [MaterialX](https://materialx.org), [OpenSubdiv](https://github.com/PixarAnimationStudios/OpenSubdiv), [OpenVDB and NanoVDB](https://www.openvdb.org), [Intel Open Image Denoise](https://www.openimagedenoise.org) and the [Vulkan SDK](https://vulkan.lunarg.com). SideFX's Karma is the reference look, and the test suite compares against it and Blender's EEVEE.

<!-- LICENSE -->
## License

Eldr is licensed under the [Apache License 2.0](LICENSE).

`third_party/nanovdb` holds NanoVDB's `PNanoVDB.h` from OpenVDB (MPL-2.0, unmodified). OpenUSD, Houdini, Vulkan, Intel OIDN and MaterialX keep their own licences.

Banner rendered in Intel Sponza (Frank Meinl, Anton Kaplanyan, Intel Sample Library, 2022) with the add-on packages by the Sponza Add-on Package Crew; CC BY 4.0. Rendered with Eldr by `docs/site/banner.py`.
