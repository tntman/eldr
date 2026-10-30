# Caustics

<!-- summary -->
Light focused through glass onto other surfaces, by progressive photon mapping (`eldr:caustics`).

- **Compare:** open `showcase/2026-10-29_13_caustics/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `caustics` vs `noCaustics`, `caustics8`, `causticsFewPhotons`.
- **Look at:** The bright focused patch in front of the glass ball and the ball's shadow around it.
<!-- summary -->

`tests/scenes/caustics.usda`: a clear glass ball on a white floor, backlit by a 45 degree sun. The ball is a caustic caster (`primvars:karma:object:causticsenable = 1`, Karma's Enable Caustics), so with `eldr:caustics` on Eldr shoots photons from the sun through it and gathers them on the floor (progressive photon mapping, `eldr:caustics`).

- `/Render/caustics` (the final-render settings, 256 samples; `eldr_final` also has caustics on) vs `/Render/noCaustics` (`eldr:caustics` 0): the bright focused patch in front of the ball, and the ball's shadow around it. Without caustics the ball casts a light, tinted shadow instead (traced translucent shadows); with them its glass blocks the shadow rays and the light arrives focused.
- `/Render/caustics8`: 8 film samples. The gather radius shrinks every sample, so the patch is softer and grainier than at 256.
- `/Render/causticsFewPhotons`: 16384 photons per sample instead of 65536: noisier per sample, the film still converges.
- `/Render/caustics` writes `caustics.exr` with the beauty `C`, the caustic light (`C<RD>.+<TS>L`) and indirect diffuse (which holds it).
- Try on the ball: `primvars:karma:object:causticsroughnessclamp` (0.2 by default, as Karma) lower for a sharper patch; `primvars:eldr:object:causticsenable = 0` to switch it off for Eldr only. On the sun: `inputs:karma:light:contributescaustics = 0`.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_13_caustics\scene.usda` (pick a settings prim: right-click, set as active; caustics are off in `eldr_default`; `caustics`, `caustics8` and `eldr_final` have them on)
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; Karma renders the same caustics with the ball's Enable Caustics (its colour limit dims them: raise Color Limit to compare)
- husk: `husk -R Eldr -s /Render/caustics showcase/2026-10-29_13_caustics/scene.usda`

Settings prims: `/Render/caustics`, `/Render/noCaustics`, `/Render/caustics8`, `/Render/causticsFewPhotons`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
