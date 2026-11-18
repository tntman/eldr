# Eldr

Eldr is a GPU renderer for USD. It plugs into Houdini's Solaris, husk, usdview and usdrecord as a Hydra render delegate and renders final frames to EXR in seconds, with Karma as the look it aims for.

## Where to start

<div class="cards">
<a class="card" href="guide/getting-started.md#install-in-houdini"><strong>Install</strong><span>Add the Eldr package to Houdini 22 and pick Eldr in the viewport.</span></a>
<a class="card" href="guide/getting-started.md#your-first-picture-in-the-viewport"><strong>First render</strong><span>Your first viewport picture, husk EXR and usdview session.</span></a>
<a class="card" href="guide/solaris.md"><strong>Rendering in Solaris</strong><span>Draft and final render settings, the Eldr tab, the USD Render ROP.</span></a>
<a class="card" href="settings.md"><strong>Settings reference</strong><span>Every render setting: default, values, USD attribute, Houdini parameter, cost.</span></a>
<a class="card" href="gallery.md"><strong>Gallery</strong><span>Eldr next to Karma, with render times. The showcase scenes let you try each feature.</span></a>
<a class="card" href="guide/karma.md"><strong>Eldr vs Karma</strong><span>Which Karma settings Eldr reads and where the two differ.</span></a>
</div>

## Sections

| Section | What's in it |
|---|---|
| [Getting started](guide/getting-started.md) | Requirements, install, first renders, install troubleshooting |
| [Rendering in Solaris](guide/solaris.md) | Render settings prims, viewport and husk renders |
| [Lights](guide/lights.md), [Materials](guide/materials.md), [Volumes](guide/volumes.md), [Cameras](guide/cameras.md) | What each kind of prim does in Eldr |
| [Sampling and denoising](guide/sampling.md), [Ray-traced effects](guide/ray-tracing.md) | Noise, quality multipliers and the [RT] effects |
| [AOVs and output](guide/aovs.md), [Crowds and performance](guide/performance.md), [Viewport](guide/viewport.md) | Output files, big scenes and interactive speed |
| [Eldr and Karma](guide/karma.md), [Gallery](gallery.md), [Showcase scenes](guide/showcase.md) | Comparisons and a scene per feature |
| [Troubleshooting](guide/troubleshooting.md) | Common problems and what to check first |
| [Settings reference](settings.md), [How Eldr works](features.md), [Houdini](houdini.md), [MaterialX coverage](materialx.md) | The reference pages |
