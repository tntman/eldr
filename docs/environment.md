# Environment variables

A normal install needs none of these: Eldr's Houdini package and `usdenv.cmd` set what Eldr needs. The rest are for debugging, timing and A/B tests. Set them before Houdini, husk or usdview starts; Eldr reads them once.

## What each one is for

| You want to | Set |
|---|---|
| See GPU time per pass, or the time of the first frame | `HDVK_TIMING=1` |
| Log frame pacing of the viewport to a CSV | `HDVK_TIMING=2`, optionally `HDVK_TIMING_CSV` |
| Find out why the film keeps restarting, or which AOVs the app asks for | `HDVK_DEBUG_AOVS=1` |
| Keep the shader cache somewhere else | `HDVK_PIPELINE_CACHE_DIR` |
| Render as on a GPU without ray tracing | `HDVK_NO_RAY_TRACING=1` |
| Run OIDN on a CUDA GPU instead of the CPU | `HDVK_OIDN_DEVICE=cuda` |
| Render UsdSkel crowds as instances | `HD_ENABLE_DEFERRED_SKINNING=1` (use `$ELDR_HUSK`, which sets it) |

## Set by the Eldr package

`eldr.json` (in the Houdini build's `houdini` folder) sets these when Houdini starts.

| Variable | Value |
|---|---|
| `PXR_PLUGINPATH_NAME` | The build folder, so USD finds the Eldr plugin |
| `PXR_AR_DEFAULT_SEARCH_PATH` | Houdini's `$HFS/bin/usd_plugins/hdx/resources/textures`, so `@StinsonBeach.hdr@` domes resolve |
| `ELDR_HUSK` | A husk wrapper with deferred skinning, for crowds: use it as the USD Render ROP's Render Command |
| `ELDR_HOUDINI` | The package folder; Houdini finds Eldr's render stats panel through it |

The package also adds its folder to `HOUDINI_PATH`, which lists Eldr in Solaris's renderer menu and adds its tabs to the LOPs.

## Read by Eldr

| Variable | What it does |
|---|---|
| `HDVK_TIMING` | `1`: prints load times and GPU time per pass (per tile for tiled renders). `2`: also writes a CSV row per frame, for viewport frame pacing |
| `HDVK_TIMING_CSV` | The CSV file for `HDVK_TIMING=2`; default `%TEMP%\eldr_timing_<time>.csv` |
| `HDVK_PIPELINE_CACHE_DIR` | Folder for the GPU program cache and the compressed-texture cache; default `%LOCALAPPDATA%\Eldr` |
| `HDVK_NO_RAY_TRACING` | `1`: behaves as if the GPU had no ray tracing; the [RT] settings are skipped |
| `HDVK_RT_EARLY_COMPILE` | `0`: compiles the ray-tracing programs on first use instead of in the background at start |
| `HDVK_OIDN_DEVICE` | Where **Denoise** runs: `cpu` (default), `cuda` or `default` (OIDN picks) |
| `HDVK_OIDN_PREFILTER` | `1`: denoises the albedo and normal guides before the beauty |
| `HDVK_DEBUG_AOVS` | `1`: prints the AOVs the app binds, and every 60 frames what restarted the film |
| `HDVK_DEBUG_HOLD` | `1`: prints per frame whether the camera moved, the still delay, the render width and the GPU time |
| `HDVK_NO_STATS` | `1`: reports only the renderer name to the app's render stats |
| `HDVK_NO_POSE_SHARE`, `HDVK_NO_SHUTTER_SHARE` | `1`: each mesh poses its skeleton, each prototype samples the shutter on its own (A/B tests of crowd sync) |
| `HDVK_SYNC_TEARDOWN` | `1`: waits for the GPU to shut down when a render ends, instead of in the background (the tests set it) |
| `HD_ENABLE_DEFERRED_SKINNING` | USD's own: `1` leaves UsdSkel crowds to Eldr, so instanced agents stay instances (much less memory). USD reads it at start, so set it before the app starts |

## Read by usdenv.cmd

`usdenv.cmd` runs USD's tools (usdview, usdrecord) with this build's Eldr. It sets `PATH`, `PYTHONPATH`, `PXR_PLUGINPATH_NAME` and `PXR_AR_DEFAULT_SEARCH_PATH` from these:

| Variable | Default |
|---|---|
| `USD_ROOT` | `C:\dev\usd-26.08-vdb`, an OpenUSD 26.08 built with OpenVDB |
| `USD_VENV` | `C:\dev\usd-venv`, the Python for usdview |
| `HDVK_BUILD` | The `build` folder next to `usdenv.cmd` |

## Read by the test tools

| Variable | Used by |
|---|---|
| `ELDR_HFS` | `tests/karma_refs.py`: the Houdini install that renders the Karma references |
| `ELDR_REFS` | `tests/karma_refs.py`: the folder of Karma reference images |
| `BLENDER` | `tests/compare_renderers.py`: Blender's executable, for EEVEE |
| `HDVK_VIEWPORT_SEQ` | `tests/test_viewport.py`: folder for its frame sequence |
| `HDVK_DUMP_SETTINGS` | `tests/test_docs.py`: prints every setting's key, label, type and default |

## Examples

GPU times of a husk render, from a Houdini command shell:

```
set HDVK_TIMING=1
husk -R Eldr -s /Render/eldr_final -o out.exr scene.usda
```

Why the viewport never settles:

```
set HDVK_DEBUG_AOVS=1
houdini scene.hip
```
