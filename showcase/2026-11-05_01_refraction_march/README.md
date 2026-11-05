# Refraction ray march

<!-- summary -->
Water and glass refract the surface their bent rays really reach, not something in front of them.
<!-- summary -->

A pool of rippled water over tiles, seen from above. Near the bottom of the image the bent rays reach tiles hidden under the pool's front deck or below the screen.

- **marchOn**: screen-space refraction follows each bent ray to the surface it reaches; where the deck hides it, it keeps the last tiles seen, so the water shows tiles down to its edge.
- **marchOff**: `eldr:refractionMarch` 0: the refraction reads the point at the estimated thickness, which lands on the deck: a flat grey band at the bottom of the water.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

Glass balls show it too: `2026-10-29_06_karma_aovs` (the green ball) and `2026-10-31_05_bsdf_graphs` (the blue glass ball).

## Open

usdview: `usdenv usdview --renderer Eldr showcase\2026-11-05_01_refraction_march\scene.usda`, then pick a settings prim. husk: `husk -R Eldr -s /Render/marchOff showcase/2026-11-05_01_refraction_march/scene.usda`.
