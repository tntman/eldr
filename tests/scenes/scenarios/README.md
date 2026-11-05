# Reflection and refraction scenarios

Small scenes of the common production cases for reflections and refraction, to compare Eldr with Karma (the reference look) and with Blender EEVEE (a raster reference).

Every scene has a camera `/cam`, a constant dome `/sky`, a key light `/key` (the furnace has only its dome) and three render settings prims:

- `eldr_default`: Eldr with nothing set.
- `eldr_rt`: Eldr with traced reflections, refraction and shadows, final gather and caustics, 64 film samples.
- `karma`: Karma, 128 path-traced samples, denoised.

Materials are MaterialX `standard_surface` with a UsdPreviewSurface output beside it, so Blender's USD importer reads them too. Textures come from `tests/scenes/textures`. `make_scenarios.py` writes the scenes; edit it and run `python tests/scenes/scenarios/make_scenarios.py` again.

Render one: `husk -R HdVkRendererPlugin -s /Render/eldr_rt tests/scenes/scenarios/car_paint.usda -o car_paint.exr` (Karma: `-R BRAY_HdKarma -s /Render/karma`), or open it in usdview with the Eldr renderer.

| Scene | What it covers | What to look for |
|---|---|---|
| `mirror_floor_roughness` | Dark dielectric floor at roughness 0, 0.2, 0.5 under a sphere and a box | Reflection in the right place, no smear or stretch; blur grows with roughness; Fresnel; the sphere's highlight and dark rim inside its reflection |
| `metal_interreflection` | Gold, silver, copper spheres touching; chrome slab behind | Each metal shows the others; dark contact gaps; reflections of reflections |
| `glass_solid_and_thin` | Solid glass sphere and slab, thin-walled pane, checker wall behind | Sphere inverts the wall, slab shifts it, thin pane doesn't bend it; nothing black |
| `glass_behind_glass` | Glass seen through glass: sphere before slab, pane before green sphere, two thin panes | The far glass still refracts and reflects; tint only where the green glass is |
| `water_over_tiles` | Closed block of rippled water (IOR 1.33) over a tiled pool | Tiles bend with the ripples; sky reflects at grazing angles; water not black or milky |
| `liquid_in_glass` | Tumbler with amber liquid, and an empty one (centimetre detail) | Wall edges, liquid colour deepening with path length, background bent through both |
| `coloured_glass` | Blue glass blocks 0.1, 0.3, 0.9 m thick (Beer-Lambert), red glass sphere, thin amber pane | Thicker is darker and more saturated; sphere darker in the middle; coloured light under each |
| `refraction_busy_background` | Glass sphere, cylinder, cube before a densely tiled wall and coloured objects | Background bent correctly; hidden objects appear in the glass; no smearing or ghosting |
| `contact_reflections` | Glossy box and chrome ball touching a glossy wall and floor | Reflections meet the objects without gap or offset; the corner reflects itself |
| `car_paint` | Dark red metallic base under a clear coat; uncoated ball for reference; softbox light | Sharp softbox and sky in the coat over the blurred base highlight; brighter edges |
| `facing_mirrors` | Two facing mirrors with objects between them | Number of repeated copies (Karma's reflect limit is 4); what fills the copies past the limit |
| `offscreen_reflections` | Mirror floor with a column, ball and box cut by the frame, a red wall behind the camera | Reflections of what is outside the frame (screen space can't see it); the red wall in the floor |
| `thin_cards` | Cut-out ring cards on a mirror floor and behind glass | Holes stay holes in the reflection and through the glass; ring-shaped shadows |
| `emissive_reflected` | Orange emissive panel, cyan emissive ball, chrome ball, glossy floor, dim lighting | Emitters bright and the right colour in the floor and the chrome; the light they cast |
| `subsurface_translucent` | Skin and jade subsurface balls, opaque ball for reference, back-lit translucent leaf | Light bleeding into the shadow side; the leaf glowing from behind |
| `furnace_grey` | Uniform white dome only: white, grey 0.5, rough white metal, clear glass, coated white | White ones read about 1 and vanish into the background, grey reads 0.5; nothing brighter than the background |
| `sun_on_chrome` | Chrome balls at roughness 0, 0.05, 0.15 under a sharp sun and a small sphere light, glossy floor | Small hot highlights that grow and dim with roughness, and again in the floor's reflection; no fireflies |
| `large_scale` | 120 m glossy plaza, tall blocks, 2 m chrome and glass balls 10 to 60 m away | Far reflections, far shadows, far glass not black, no acne or banding |
| `tiny_scale` | Millimetre beads on a 20 cm tile, camera 6 cm away | Same look as at metre scale: no acne, no detached shadows, glass not black |

`tests/test_scenarios.py` (ctest `scenarios`, label slow) renders seven of them with Eldr, default and traced, and checks what must hold whatever the renderer: the ball's reflection sits where its mirror image projects with no smear beside it, glass isn't black, the furnace spheres read their albedo, the constant dome shows behind them, thicker blue glass is bluer, a closed thin pane tints twice, glass behind glass keeps its shape, the liquid fills its tumbler and facing mirrors turn black past the reflect limit.

To compare with Karma and EEVEE: `hython tests/compare_renderers.py tests/scenes/scenarios/<name>.usda <outdir> --eldr eldr_default,eldr_rt --karma-dome-hidden` (the dome hidden from Karma's camera; drop the flag to see it behind the scene, as Eldr draws it).

## Known differences

Compared with Karma (husk, 640x360). Fix lanes remove an item when it's fixed.

- Without ray tracing: refraction doesn't invert or bend the background through a glass ball, and water reads milky at grazing angles; screen-space reflections leave a grainy cone in the lower half of chrome balls.
