# Tutorial: your first scene

This tutorial builds a small scene in Solaris from empty, lights and shades it, and renders a final EXR with Eldr. It takes about 15 minutes. You need Eldr installed ([Getting started](getting-started.md#install-in-houdini)).

Each step changes one thing, so you see what it does. The node names are Houdini's own; Eldr needs no special nodes.

## 1. The objects

1. Make a LOP network (the **Solaris** desktop, `/stage`).
2. Add a **Cube** LOP. Set **Scale** to `10, 0.1, 10`: this is the floor.
3. Wire a **Sphere** LOP below it. Raise **Translate Y** until the sphere sits on the floor.
4. Set the display flag on the Sphere LOP.

You should see: the floor and the sphere in the viewport, rendered by Houdini's own GL renderer.

## 2. Switch to Eldr

1. In the viewport, open the renderer menu at the top right and pick **Eldr**.

You should see: the same objects, rendered by Eldr. While you move the camera the image is soft and fast; when you stop, Eldr adds samples for a second or two (the *film*) and the image settles. Until the scene has lights of its own, the viewport's lighting mode decides how they are lit; the next step adds real lights.

## 3. Lights

1. Wire a **Dome Light** LOP below the sphere. Set its **Texture** to `StinsonBeach.hdr` (an HDRI that ships with Houdini; Eldr's package makes the name resolve).
   You should see: sky light from every side and soft contact shadows under the sphere.
2. Wire a **Light** LOP below the dome. Set **Light Type** to **Rectangle**, move and rotate it so it points at the sphere from above and to the side, and raise **Exposure** until it reads as the key light.
   You should see: a shadow on the floor. A bigger light gives a softer shadow, as in Karma.

The rectangle itself isn't visible in the camera: Eldr hides light shapes unless **Render Light Geometry** is on (Light LOP > Karma or Eldr tab), as Karma does. See [Lights](lights.md).

## 4. A material

1. Add a **Material Library** LOP below the lights. Dive in and add a **Karma Material Builder**.
2. Inside the builder, on the **MtlX Standard Surface** node, set **Base Color** to a colour you like, and **Specular Roughness** to `0.3`.
3. Back in `/stage`, wire an **Assign Material** LOP below the library. Set **Primitives** to the sphere and **Material Path** to the new material.

You should see: the sphere in colour, with a soft highlight from the rect light and the sky reflected in it.

Any MaterialX or Karma material renders the same way: see [Materials](materials.md).

## 5. A camera

1. Wire a **Camera** LOP below the Assign Material.
2. In the viewport's camera menu (top right), look through the new camera and frame the shot. Lock the camera to the view, or set **Translate** and **Rotate** on the node.

You should see: the image through the camera. Its **Exposure**, **ISO**, **Shutter** and **F-Stop** scale the image as in Karma (see [Cameras](cameras.md)).

## 6. Draft and final render settings

Eldr has no quality presets: a final is a Render Settings node with the expensive settings ticked one by one.

1. Wire a **Render Settings** LOP below the camera and name it `draft`. Set **Camera** to your camera and the **Resolution** you want. Leave the **Eldr** tab alone.
2. Wire a second **Render Settings** LOP below it and name it `final`. Same camera and resolution. On its **Eldr** tab, set the [final settings](solaris.md#draft-and-final-two-render-settings-nodes) to **Set or Create** and tick them: Film Samples 256, Denoise, Global Illumination, Final Gather, the ray-traced shadows, reflections and refraction.
3. In the viewport's camera / render settings menu, pick the `final` prim to see it, then switch back to `draft` while you work.

You should see: with `final`, bounce light on the sphere's shadow side from the floor, sharper reflections, and a still frame that takes a few seconds longer to settle.

> **Tip:** Ray-traced settings need a GPU with Vulkan ray tracing. Without it they do nothing and the image stays as the draft.

## 7. Render to disk

1. Wire a **USD Render ROP** LOP below the last node.
2. Set **Render Settings** to the `final` prim, and **Output Picture** to a file name ending in `.exr`.
3. Click **Render to Disk**.

You should see: husk's progress, then the EXR. It holds linear, untonemapped colour, as Karma's does: view it with an OCIO view (ACES or sRGB) in MPlay or your comp.

To compare with Karma, render the same node with Karma as the ROP's renderer: Eldr aims for the same lighting and exposure.

## Next steps

- [Rendering in Solaris](solaris.md): every group of the Eldr tab.
- [AOVs and output](aovs.md): light splits, light groups and Cryptomatte in the same EXR.
- [Sampling and denoising](sampling.md): where noise comes from and which setting removes it.
- [Showcase scenes](showcase.md): a finished scene for every feature, each with `eldr_default`, `eldr_final` and `karma`.
