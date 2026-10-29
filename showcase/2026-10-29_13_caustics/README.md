# Caustics

`tests/scenes/caustics.usda`: a clear glass ball on a white floor, backlit by a 45 degree sun. The ball is a caustic caster (`primvars:karma:object:causticsenable = 1`, Karma's Enable Caustics), so in final quality Eldr shoots photons from the sun through it and gathers them on the floor (progressive photon mapping, `eldr:caustics`).

- `/Render/caustics` vs `/Render/noCaustics`: the bright focused patch in front of the ball, and the ball's shadow around it. Without caustics the ball casts a light, tinted shadow instead (traced translucent shadows); with them its glass blocks the shadow rays and the light arrives focused.
- `/Render/caustics8`: 8 film samples. The gather radius shrinks every sample, so the patch is softer and grainier than at 256.
- `/Render/causticsFewPhotons`: 16384 photons per sample instead of 65536: noisier per sample, the film still converges.
- `/Render/caustics` writes `caustics.exr` with the beauty `C`, the caustic light (`C<RD>.+<TS>L`) and indirect diffuse (which holds it).
- Try on the ball: `primvars:karma:object:causticsroughnessclamp` (0.2 by default, as Karma) lower for a sharper patch; `primvars:eldr:object:causticsenable = 0` to switch it off for Eldr only. On the sun: `inputs:karma:light:contributescaustics = 0`.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_13_caustics\scene.usda` (pick a settings prim: right-click, set as active; caustics run in final quality, or set `eldr:caustics` to 2)
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; Karma renders the same caustics with the ball's Enable Caustics (its colour limit dims them: raise Color Limit to compare)
- husk: `husk -R Eldr -s /Render/caustics showcase/2026-10-29_13_caustics/scene.usda`

Settings prims: `/Render/caustics`, `/Render/noCaustics`, `/Render/caustics8`, `/Render/causticsFewPhotons`.
