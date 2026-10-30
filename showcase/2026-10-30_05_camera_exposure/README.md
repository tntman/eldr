# Camera exposure

Eldr applies the USD camera's exposure as Karma does: the image times `HdCamera::GetLinearExposureScale()`, which UsdGeomCamera computes from `exposure`, `exposure:iso`, `exposure:time`, `exposure:fStop` and `exposure:responsivity`. Beauty and light AOVs (emission, the diffuse and glossy splits, light groups) scale; depth, normals, ids and albedo do not.

What to look at: `/cam` here is exposure 1 at ISO 200 (4x). `/Render/camera` (the default) is 4x brighter than `/Render/ignored` (`eldr:cameraExposure` off).

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_05_camera_exposure\scene.usda`, then pick a settings prim, or flip Camera Exposure in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/camera scene.usda`.
