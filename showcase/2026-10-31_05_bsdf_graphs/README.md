# MaterialX BSDF graphs

<!-- summary -->
MaterialX `surface` shaders built from pbrlib BSDFs (mix, layer, add, multiply of diffuse, dielectric, conductor, sheen and subsurface BSDFs, an EDF), Lama's BSDFs, and glTF PBR, on spheres.

- **Compare:** open `showcase/2026-10-31_05_bsdf_graphs/scene.usda` and compare `eldr_default` vs `karma`; the feature's A/B nodes: `graphsOn` vs `graphsOff` (`eldr:materialGraphs` off).
- **Look at:** Each sphere against Karma: the mixed diffuse is the average of red and green, the coated gold has a sharp clear highlight over a blurred gold one, the masked sphere is red paint on the left and gold on the right.
<!-- summary -->

Eldr maps each BSDF graph onto the lobes of one standard_surface (docs/materialx.md, "BSDF graphs").

- Top row: a 50/50 mix of red and green diffuse; plastic (a dielectric layered over a blue diffuse); coated gold (a dielectric over a conductor: a coat); red paint and gold mixed by the object-space x position (a graph-driven mix weight); sheen over a purple diffuse; tinted glass (a dielectric in RT mode).
- Bottom row: subsurface under a specular layer; a dark diffuse with an orange uniform EDF (multiplied by 2); Lama coated copper (LamaLayer of LamaDielectric over LamaConductor, artistic Fresnel); Lama diffuse plus half a Lama sheen (LamaAdd); glTF PBR iridescence; glTF PBR brushed metal (anisotropy).

A/B: `/Render/graphsOn` (default) vs `/Render/graphsOff` (`eldr:materialGraphs` off: the masked sphere loses its graph-driven mix and the Lama spheres their roughness graphs).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-31_05_bsdf_graphs\scene.usda`
- husk: `husk -R Eldr -s /Render/graphsOn showcase/2026-10-31_05_bsdf_graphs/scene.usda`; Karma: `husk -s /Render/karma ...`
- usdrecord: `usdenv usdrecord --renderer Eldr --disableCameraLight --camera /cam showcase\2026-10-31_05_bsdf_graphs\scene.usda out.png`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
