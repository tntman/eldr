# Offline samples per execute

`tests/scenes/emissive.usda` with GI, 64 film samples. Offline renders (husk, usdrecord) render film samples back to back inside one execute for up to `eldr:offlineBatchSeconds` and read back only the last one, so the app's wait between executes (husk: about 0.1 s per sample) goes away. The images are the same; only the wall time differs.

- `/Render/batched`: `eldr:offlineBatchSeconds` 1 (the default).
- `/Render/oneSamplePerExecute`: 0, one sample per execute.

Compare the wall time of the two. husk's progress updates once per batch.

## Open

- husk: `husk -R Eldr -s /Render/<name> -o out.exr showcase/2026-10-29_02_offline_batch/scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --renderSettingsPrimPath /Render/<name> showcase\2026-10-29_02_offline_batch\scene.usda out.png`
- usdview and Houdini's viewport are interactive: the setting does nothing there.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
