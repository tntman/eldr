# Planar blur, light textures in traced paths

- `/Render/sharp` vs `/Render/blurred`: `eldr:planarBlur` 0 vs 1. A rougher mirror floor blurs its reflection (12-tap disc, radius ~ roughness squared). Change the floor's roughness to compare.
- Light textures now also tint traced hits, probes/GI, fog and volumes (`eldr:lightTextures`): add `inputs:texture:file` to a RectLight and turn on GI or fog to see the pattern in bounce light.
- Open: `usdenv usdview --renderer Eldr showcase\2026-10-29_03_planar_blur_lighttex\scene.usda`, or `husk -R Eldr -s /Render/blurred scene.usda`.
- Not done: refraction in planar mirrors, contact-hardening blur.
