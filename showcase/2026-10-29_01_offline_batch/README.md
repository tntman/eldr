# Offline samples per execute

`tests/scenes/emissive.usda` with GI, 64 film samples. Offline renders (husk, usdrecord) render film samples back to back inside one execute for up to `eldr:offlineBatchSeconds` and read back only the last one, so the app's wait between executes (husk: about 0.1 s per sample) goes away. The images are the same; only the wall time differs.

- `/Render/batched`: `eldr:offlineBatchSeconds` 1 (the default).
- `/Render/oneSamplePerExecute`: 0, one sample per execute as before.

Compare the wall time of the two. husk's progress updates once per batch.

## Open

- husk: `husk -R Eldr -s /Render/<name> -o out.exr showcase/2026-10-29_01_offline_batch/scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --renderSettingsPrimPath /Render/<name> showcase\2026-10-29_01_offline_batch\scene.usda out.png`
- usdview and Houdini's viewport are interactive: the setting does nothing there.
