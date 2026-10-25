# Eldr goals

Eldr (code name HdVk) renders images to EXR for VFX work. It is a fast raster
renderer, which is the whole point, but it is not a game renderer: a final
frame taking 0.1 s or 10 s is fine as long as the image is good.

## What that means in practice

- **Final frames (usdrecord, husk) favour quality over time per frame:** more
  accumulated samples, adaptive convergence, full-resolution effects.
- **Interactive work in usdview stays fast.**
- **EXR output with all AOVs is core, not optional.**
- **Features are judged by how they look converged** in a still frame and in
  motion-blurred sequences, not by single-frame noise at 60 fps.

## Ground rules

- **No Houdini dependency.** Eldr builds and runs against plain OpenUSD.
  Houdini-only code lives in optional modules of the Houdini build.
- **Every render feature gets a Hydra render setting** (`hdvk:*`), so it can
  be A/B tested and turned off for performance.
- **The display name is Eldr; ids stay HdVk** (plugin, tokens, settings).
