# Reflections on a mirror floor

<!-- summary -->
A ball on a mirror-like floor reflects as a ball, with its highlights, and reflections reflect again up to a limit.
<!-- summary -->

A glossy purple ball on an orange, glass-like floor (roughness 0), lit by an HDRI of a beach and a sun.

- **rtOff**: screen-space reflections. The floor shows the ball where the screen sees it; its hidden underside comes from the dome. The back faces' depth (`eldr:ssrBackfaceDepth`) tells SSR how thick the ball is, so rays under it hit it and rays past it go on: a round reflection down to the contact.
- **backfaceOff**: rtOff without `eldr:ssrBackfaceDepth`: every surface counts as one pixel thick, so rays that go under the ball miss it and the reflection's lower edge comes from the dome.
- **rtOn**: traced reflections. The reflected ball has the sun's highlight, a brighter rim, and the floor reflected in it.
- **reflectLimit1**: traced, one bounce: the reflected ball reflects only the dome.
- **reflectLimit4**: traced, four bounces (`eldr:rtReflectionBounces`), Karma's default.
- **karmaReflectLimit1**: Karma's `karma:global:reflectlimit` 1, used while `eldr:rtReflectionBounces` is unset.
- **planar**: planar reflections: the floor mirrors a second render of the scene, the ball's underside included, without ray tracing.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

Ray tracing costs more on GPUs without RT cores.

## Open

Houdini: `scene.hip`. usdview: `usdenv usdview --renderer Eldr showcase\2026-11-04_01_reflections\scene.usda`, then pick a settings prim. husk: `husk -R Eldr -s /Render/rtOn showcase/2026-11-04_01_reflections/scene.usda`.

## Houdini scene

Open `scene.hip`. A Sublayer LOP loads `geo.usda`, the scene's geometry only. Everything else is a Houdini node you can change: the camera (Camera LOP), each light (Light, Distant Light, Dome Light LOPs), the materials (a Material Library with MaterialX, USD Preview Surface and Karma VOPs), render products and AOVs (Render Product, Render Var LOPs), and one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

A value a node has no parameter for sits on the node as an extra parameter, or on an Edit Properties LOP right after it. A material Houdini's VOPs can't rebuild exactly comes from `scene.usda` through a Reference LOP.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). `scene.usda` holds the same scene for usdview and husk (it sublayers `geo.usda`): `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda` (`--check` compares the hip's stage with `scene.usda`).
