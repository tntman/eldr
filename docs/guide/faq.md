# FAQ

## Is Eldr a path tracer?

No. Eldr is a GPU raster renderer: a still frame adds samples (the *film*) until it converges, so soft shadows, depth of field and motion blur resolve exactly. Ray-traced shadows, reflections, refraction, GI and caustics are extra settings, off by default. For a path-traced reference, render the same stage with Karma.

## Do I need an RTX GPU?

No. Eldr needs Vulkan 1.3. Only the settings marked [RT] need Vulkan ray tracing; without it Eldr skips them and renders the rest.

## Which systems does it run on?

Windows, 64-bit, with Houdini 22 (22.0.466) or OpenUSD 26.08. A prebuilt package for Houdini 22.0.466 is on the [Releases](https://github.com/tntman/eldr/releases/latest) page; other versions build from source ([Houdini](../houdini.md#build)).

## Which Houdini licence do I need?

husk, and so the USD Render ROP, needs a licence above Apprentice.

## Do I need Eldr materials or lights?

No. Eldr renders the stage Karma renders: UsdLux lights, MaterialX, Karma Material Builder networks, UsdPreviewSurface, OpenVDB volumes. See [Materials](materials.md).

## Does it read my Karma settings?

Many of them: the quality multipliers, colour limits, reflection and refraction limits, the denoiser, and per-object visibility, motion blur, dicing and light attributes. [Eldr and Karma](karma.md#karma-settings-eldr-reads) lists them. When both are set, Eldr's own setting wins.

## Why does my render look different from Karma's?

Eldr aims for Karma's lighting, exposure and colour, but approximates some effects. [Eldr and Karma](karma.md#known-differences) lists the known differences, and the [Gallery](../gallery.md) shows both side by side.

## Is there a draft or final quality preset?

No. The viewport, husk and usdrecord use the same settings, so a frame looks the same everywhere. A final is a Render Settings node with the expensive settings ticked: see [Rendering in Solaris](solaris.md#draft-and-final-two-render-settings-nodes).

## How long does a final frame take?

Seconds at 1080p on a recent GPU, more with ray tracing and many samples. The [Gallery](../gallery.md) lists measured times next to Karma's.

## Why is the first frame slow?

Eldr compiles its GPU programs the first time it sees a feature set, and caches them on disk. The next start is fast.

## Can I render without Houdini?

Yes: usdview and usdrecord, through `usdenv.cmd` ([Getting started](getting-started.md#usdview-and-usdrecord)).

## Eldr or HdVk?

The same renderer. **Eldr** is the name in every menu; `HdVkRendererPlugin` is the plugin id. husk and scripts accept either.
