# Reflections in motion blur

An orange cube races across the mirror floor in front of the chrome ball (3 units a frame, shutter open half a frame). Render frame 2.

- `/Render/planar`: the floor's planar reflection blurs as the cube does (`eldr:planarMotionBlur` on: each film sample renders the mirror at its shutter time).
- `/Render/frozen`: `eldr:planarMotionBlur` off: a sharp reflection under a blurred cube (the old look).
- `/Render/traced`: `eldr:rtReflections` 2: floor and ball reflect from each sample's TLAS, so both blur.
- The chrome ball under `planar`/`frozen` is probe-lit (`eldr:reflectionProbe`): probes are captured at the frame's time, so the cube in the ball stays sharp. A capture is six renders and a CPU prefilter, too slow per film sample; use traced reflections for blurred reflections on curved surfaces.
- Open: `usdenv usdview --renderer Eldr showcase\2026-10-29_09_reflection_motion_blur\scene.usda` (set the frame to 2), or `husk -R Eldr -s /Render/planar -f 2 scene.usda`.
