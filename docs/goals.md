# Eldr goals

Eldr is a fast raster renderer for VFX: final frames to EXR. It is not a game renderer; 0.1 s or 10 s per frame is fine if the image is good.

| Area | Goal |
|---|---|
| Final frames (usdrecord, husk) | Quality over time per frame: more film samples, adaptive convergence, full-resolution effects |
| Interactive (usdview) | Stays fast |
| Output | EXR with all AOVs is core |
| Judging features | By how they look converged, in stills and motion-blurred sequences |

## Ground rules

- **No Houdini dependency:** Eldr builds and runs on plain OpenUSD. Houdini-only code is in optional modules of the Houdini build.
- **Every render feature has an `eldr:*` setting** to turn it on or off for A/B tests and speed.
- **Names:** the display name is Eldr; plugin ids stay `HdVk`.
