# Volumes

Eldr renders OpenVDB volumes (smoke, clouds, fire, explosions) and a simple height fog. Volumes are marched on the GPU (as NanoVDB), lit by every light, and cast coloured shadows on surfaces, fog and each other.

> **Where in Houdini:** Volumes come in through a **SOP Import LOP** (VDB or Houdini volumes, no files needed) or a **Volume LOP** / Sublayer of `.vdb` files. Materials go on with a **Material Library LOP** (MaterialX `volume`, or Karma's `kma_volume`). Per-volume controls are on the **Render Geometry Settings LOP**.

![Houdini: SOP Import LOP bringing a pyro VDB into Solaris](../images/houdini/sop_import_volume.png)

## Without a material

A volume without a material renders like Karma's: the `density` field scatters white light, no absorption, the same in every direction. That is what you get from a plain SOP Import of a smoke sim.

![A cloud without a material, as Karma renders it (29_08, newPhase)](../images/volume_default.jpg)

**Volume Default Fields** also reads `temperature` (as fire), `Cd` (colour) and `emission` without a material.

> **Showcase:** open `showcase/2026-10-29_08_volume_default_material/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `newPhase` (the default, isotropic) vs `oldPhase` (`eldr:volumeAnisotropy` 0.2, forward scattering).

## With a material

Eldr reads MaterialX `volume` materials (anisotropic_vdf, absorption_vdf, uniform_edf) and Karma's `kma_volume`:

- Constant colours and field reads (`geompropvalue`), multiplied together.
- Ramps and remaps of one field each, up to three per material: Houdini's MtlX Color and Float Ramps, Karma Ramp Const, MaterialX `ramp`, `remap`, `range`, `smoothstep` and maths.
- Blackbody colour from a temperature field: fire.
- Absorption and scattering per colour channel: coloured smoke and coloured shadows.

![Coloured smoke casts an orange shadow (29_05, maps)](../images/volume_shadows_on.jpg)
![Volume Shadows off (29_05, shadowsOff)](../images/volume_shadows_off.jpg)

> **Warning:** Karma's **Pyro Shader** isn't read. Eldr renders such a volume with its density only and warns. Build the look with a MaterialX volume or Karma Volume material instead.

> **Showcase:** open `showcase/2026-10-29_05_coloured_volume_shadows/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `maps` vs `shadowsOff` (`eldr:volumeShadows`), `traced` (`eldr:rtShadows`), `halfResolution` (`eldr:volumeResolution` 2).

## Multiple scattering: Volume Limit

Bright clouds get their glow from light that bounces many times inside. Karma controls that with **Volume Limit**; Eldr reads the same values and fits its look to Karma's at each limit:

- **Render Geometry Settings**, Volume Limit (Eldr or Karma tab): the limit of the volumes it targets.
- **Render Settings**, Volume Limit (Eldr or Karma tab): the limit of every other volume. 0 (Karma's default) is single scattering.

Each volume uses its own limit, so a thick cloud at 16 and thin smoke at 0 render side by side as Karma renders them.

![Volume limit 1: single scattering (29_26, volumeLimit1)](../images/volume_limit1.jpg)
![Volume limit 16: the cloud brightens as Karma's (29_26, volumeLimit16)](../images/volume_limit16.jpg)

> **Showcase:** open `showcase/2026-10-29_26_volume_multiple_scattering/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `volumeLimit1` vs `volumeLimit4` vs `volumeLimit16`. The small ball keeps its own limit 16 in all of them.

## Velocity blur

A volume with **Velocity Blur** on (Render Geometry Settings) blurs along its `vel` field when motion blur samples run: a vector VDB, or Houdini's `vel.x`, `vel.y`, `vel.z`. **Volume Velocity Blur Scale** strengthens or weakens it.

![Velocity blur along vel (29_18, blur)](../images/volume_blur.jpg)
![Volume Velocity Blur off (29_18, sharp)](../images/volume_sharp.jpg)

> **Showcase:** open `showcase/2026-10-29_18_volume_motion_blur/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `blur` vs `sharp` (`eldr:volumeVelocityBlur` off).

## Fog

**Volumetric Fog** (off) fills the scene with height fog: thicker near the ground, lit by every light, with god rays from the sun and cones from spots. Set **Fog Density**, **Fog Height Falloff** and **Fog Color**. There is no USD fog prim, so fog lives only in the render settings.

![Fog with sun shafts between the pillars and a spot's cone (30_12, fog)](../images/fog_on.jpg)
![Fog off (30_12, fogOff)](../images/fog_off.jpg)
![Dense warm fog without height falloff (30_12, fogDense)](../images/fog_dense.jpg)

> **Showcase:** open `showcase/2026-10-30_12_fog/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `fog` vs `fogOff`, `fogDense`.

## Speed

Volumes can be the slowest part of a frame. To speed them up:

| Setting | Effect |
|---|---|
| **Volume Resolution** 2 | March at half resolution: about 3x cheaper, slightly softer edges |
| **Volume Step Size** | Longer steps are faster, but thin wisps may vanish |
| **Volume Shadow Grid** (on) | Each volume's shadowing is kept on a small grid instead of marched every step: 3-4x cheaper |
| **Volume Quality** | Fewer or more shadow steps (Karma's Volume Quality) |

> **Showcase:** open `showcase/2026-10-30_03_volume_shadow_grid/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `grid` (64, default) vs `exact` (0) vs `grid32`.

> **Showcase:** open `showcase/2026-10-29_20_volume_light_shadow_stride/scene.hip`, compare `eldr_default` vs `eldr_final` vs `karma`, then `stride2` (default) vs `stride1` vs `stride4`.

## Limitations

- Volumes aren't pickable in the viewport.
- `op:` paths from SOP Import resolve only inside Houdini: for husk on a saved file, write the stage with a USD ROP first (volumes go to `.vdb` files next to it).
- Multiple scattering reaches the sun and the dome, not local lights.
- More on the [How Eldr works](../features.md#volumes-and-fog) and [Houdini](../houdini.md#volumes) pages.
