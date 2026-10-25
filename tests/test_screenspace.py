"""Headless check of HdVk's screen-space effects through Hydra: the
thickness estimator (SSR at grazing angles), SSGI's visibility bitmask,
denoised glossy SSR, its footprint-filtered hits, its precision, SSGI's energy and
tile order, back-face hits and the denoiser's sample dimensions.
Run: usdenv.cmd python tests\\test_screenspace.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL, UsdLux

SIZE = 400

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def render(stage, settings={}, frames=4):
    """The stage through /cam with HdVk, as a QImage; no TAA (unless set), fixed exposure."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for key, value in {"hdvk:taa": False, "hdvk:autoExposure": False, **settings}.items():
        engine.SetRendererSetting(key, value)
    frustum = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default()).frustum
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.clearColor = Gf.Vec4f(0, 0, 0, 1)
    for _ in range(frames):
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    del engine  # release GL resources while the context is still current
    return image


def mean(image, x0, y0, x1, y1):
    """Mean 0..255 green over [x0, x1) x [y0, y1) (y down)."""
    total = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            total += image.pixelColor(x, y).green()
    return total / ((x1 - x0) * (y1 - y0))


# The Cornell box from floor height: the chrome sphere's lower half
# reflects the floor, a strip a few pixels high on screen. Marching even
# steps, SSR skips over it on most rays (black grain); the estimator
# searches the jump in depth past it pixel by pixel.
stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "cornell.usda"))
cam = UsdGeom.Xformable(stage.GetPrimAtPath("/cam"))
cam.ClearXformOpOrder()
cam.AddTranslateOp().Set(Gf.Vec3d(0.2, 0.06, 2.6))
region = (200, 160, 280, 200)  # the sphere's lower half
off = mean(render(stage, {"hdvk:ssgiThickness": False}), *region)
on = mean(render(stage, {"hdvk:ssgiThickness": True}), *region)
print("SSR at floor height: the floor's reflection %.1f without the thickness estimator, %.1f with it" % (off, on))
expect(on > 1.3 * off, "the thickness estimator fills SSR's grain at grazing angles")

# SSGI's visibility bitmask: a thin pole far in front of a wall under a
# uniform dome. The highest horizon takes the pole for a wall that tall
# and darkens a halo on the wall around its image; the bitmask hides only
# the few sectors the pole covers.
stage = Usd.Stage.CreateInMemory()
wall = UsdGeom.Mesh.Define(stage, "/wall")
wall.CreatePointsAttr([(-4, -4, 0), (4, -4, 0), (4, 4, 0), (-4, 4, 0)])
wall.CreateFaceVertexCountsAttr([4])
wall.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
wall.CreateDisplayColorAttr([(0.8, 0.8, 0.8)])
pole = UsdGeom.Cube.Define(stage, "/pole")
pole.AddTranslateOp().Set(Gf.Vec3d(0, 0, 1.5))
pole.AddScaleOp().Set(Gf.Vec3f(0.05, 4, 0.05))
UsdLux.DomeLight.Define(stage, "/dome")
UsdGeom.Camera.Define(stage, "/cam").AddTranslateOp().Set(Gf.Vec3d(0, 0, 5))


def halo(bitmask, thickness=0.25):
    """How much darker the wall is just beside the pole's image than at the edge."""
    image = render(stage, {"hdvk:taa": True, "hdvk:ssgi": True, "hdvk:ssgiRadius": 3.0, "hdvk:ssgiBitmask": bitmask,
                           "hdvk:ssgiBitmaskThickness": thickness},
                   frames=20)
    row = [mean(image, x, SIZE // 2 - 2, x + 4, SIZE // 2 + 2) for x in range(0, SIZE // 2 - 24, 4)]
    return row[0] - min(row)


horizon, bitmask = halo(False), halo(True)
print("SSGI halo around a thin pole: %.1f with the highest horizon, %.1f with the bitmask" % (horizon, bitmask))
expect(horizon > 2 and bitmask < 0.5 * horizon, "SSGI's bitmask doesn't darken a halo around a thin object")
thick = halo(True, 4.0)
print("SSGI bitmask halo: %.1f at hdvk:ssgiBitmaskThickness 0.25, %.1f at 4" % (bitmask, thick))
expect(thick > bitmask + 1, "hdvk:ssgiBitmaskThickness: thicker objects hide more light")

# Denoised glossy SSR: reflections.usda's floor at roughness 0.25. One
# GGX-sampled ray per pixel and frame, denoised: the columns' glossy
# reflections show (brighter than the dome's light alone, without SSR),
# smooth in a single frame (no TAA).
stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "reflections.usda"))
stage.GetPrimAtPath("/mtl/polished/pbr").GetAttribute("inputs:roughness").Set(0.25)
column = (64, 220, 80, 320)  # the left column's reflection


def roughness(image, x0, y0, x1, y1):
    """Mean absolute 0..255 difference of horizontally adjacent pixels."""
    total = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            total += abs(image.pixelColor(x + 1, y).green() - image.pixelColor(x, y).green())
    return total / ((x1 - x0) * (y1 - y0))


bright = {"hdvk:exposureCompensation": 2.0}
dome = mean(render(stage, {"hdvk:ssr": False, **bright}), *column)
denoised = render(stage, {"hdvk:ssrDenoise": True, **bright}, frames=8)
glossy = mean(denoised, *column)
grain = roughness(denoised, 100, 240, 160, 300)
print("glossy SSR: the column's reflection %.1f (dome alone %.1f), grain %.2f" % (glossy, dome, grain))
expect(glossy > dome + 5, "denoised SSR shows a glossy floor's reflections")
expect(grain < 3, "denoised SSR is smooth without TAA")

# Footprint-filtered SSR hits (hdvk:ssrHitFilter): at roughness 0.25 the
# mirror ray reads the mip of last frame's image as wide as the lobe at
# the hit, so the columns' reflections blur (smaller steps between
# neighbouring pixels across their edges) instead of staying mirror-sharp.
edges = (40, 230, 110, 300)  # the left column's reflection and its edges
sharp = roughness(render(stage, {**bright, "hdvk:ssrHitFilter": False}), *edges)
blurred = roughness(render(stage, {**bright, "hdvk:ssrHitFilter": True}), *edges)
print("SSR hit filter: steps across the column's reflection %.2f without, %.2f with" % (sharp, blurred))
expect(blurred < 0.8 * sharp, "hdvk:ssrHitFilter blurs glossy reflections by the ray's footprint")

# SSR's precision (EEVEE 5.2's fix): a mirror facing the camera, far from
# it, has nothing in front of it to reflect: its rays head back towards the
# camera over its own depth, where equal depths rounded apart would hit it
# again. SSR on then looks like SSR off.
from pxr import Sdf, UsdShade
stage = Usd.Stage.CreateInMemory()
mirror = UsdGeom.Mesh.Define(stage, "/mirror")
mirror.CreatePointsAttr([(-50, -50, 0), (50, -50, 0), (50, 50, 0), (-50, 50, 0)])
mirror.CreateFaceVertexCountsAttr([4])
mirror.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
mirror.AddRotateXYZOp().Set(Gf.Vec3f(10, 25, 0))
chrome = UsdShade.Material.Define(stage, "/chrome")
pbr = UsdShade.Shader.Define(stage, "/chrome/pbr")
pbr.CreateIdAttr("UsdPreviewSurface")
pbr.CreateInput("metallic", Sdf.ValueTypeNames.Float).Set(1.0)
pbr.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.0)
chrome.CreateSurfaceOutput().ConnectToSource(pbr.ConnectableAPI(), "surface")
UsdShade.MaterialBindingAPI.Apply(mirror.GetPrim()).Bind(chrome)
UsdLux.DomeLight.Define(stage, "/dome").CreateTextureFileAttr("StinsonBeach.hdr")
UsdGeom.Camera.Define(stage, "/cam").AddTranslateOp().Set(Gf.Vec3d(0, 0, 100))
off, on = render(stage, {"hdvk:ssr": False}), render(stage, {"hdvk:ssr": True})
selfHits = sum(abs(on.pixelColor(x, y).green() - off.pixelColor(x, y).green()) > 2
               for y in range(0, SIZE, 2) for x in range(0, SIZE, 2))
print("SSR over a mirror facing the camera: %d pixels (of a quarter) hit something" % selfHits)
expect(selfHits == 0, "SSR: rays towards the camera don't hit their own surface")

# SSGI's energy (the white furnace): white diffuse walls, floor and a box
# under a uniform dome. Light that SSGI takes for a hidden share of the
# sky comes from surfaces as bright as the sky, so no pixel may end up
# brighter than with the sky unoccluded (no AO, no SSGI); a 5% floor on
# the slices' weights made grazing surfaces up to 2.3% too bright.
stage = Usd.Stage.CreateInMemory()
white = UsdShade.Material.Define(stage, "/white")
pbr = UsdShade.Shader.Define(stage, "/white/pbr")
pbr.CreateIdAttr("UsdPreviewSurface")
pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set((1, 1, 1))
pbr.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(1.0)
pbr.CreateInput("ior", Sdf.ValueTypeNames.Float).Set(1.0)
white.CreateSurfaceOutput().ConnectToSource(pbr.ConnectableAPI(), "surface")
for name, points in (("floor", [(-4, 0, -4), (-4, 0, 4), (4, 0, 4), (4, 0, -4)]),
                     ("back", [(-4, 0, -2), (4, 0, -2), (4, 2, -2), (-4, 2, -2)]),
                     ("left", [(-2, 0, -4), (-2, 2, -4), (-2, 2, 4), (-2, 0, 4)])):
    quad = UsdGeom.Mesh.Define(stage, "/" + name)
    quad.CreatePointsAttr(points)
    quad.CreateFaceVertexCountsAttr([4])
    quad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    UsdShade.MaterialBindingAPI.Apply(quad.GetPrim()).Bind(white)
box = UsdGeom.Cube.Define(stage, "/box")
box.AddTranslateOp().Set(Gf.Vec3d(0.5, 0.5, -0.5))
UsdShade.MaterialBindingAPI.Apply(box.GetPrim()).Bind(white)
UsdLux.DomeLight.Define(stage, "/dome")
cam = UsdGeom.Camera.Define(stage, "/cam")
cam.AddTranslateOp().Set(Gf.Vec3d(1.8, 1.6, 3.2))
cam.AddRotateXYZOp().Set(Gf.Vec3f(-8, 25, 0))
cam.CreateFocalLengthAttr(24)
linear = {"hdvk:taa": True, "hdvk:tonemapper": "none", "hdvk:exposureCompensation": -1.0, "hdvk:ssr": False,
          "hdvk:bloomIntensity": 0.0, "hdvk:adaptiveThreshold": 0.0, "hdvk:adaptiveFinalThreshold": 0.0}
sky = render(stage, {**linear, "hdvk:ambientOcclusion": False}, frames=70)
for bitmask in (False, True):
    gi = render(stage, {**linear, "hdvk:ssgi": True, "hdvk:ssgiBitmask": bitmask}, frames=70)
    brighter = darker = lit = 0
    for y in range(0, SIZE, 2):
        for x in range(0, SIZE, 2):
            s, g = sky.pixelColor(x, y).green(), gi.pixelColor(x, y).green()
            if s > 0:
                lit += 1
                brighter += g > s + 1
                darker += g < s - 6
    print("SSGI %s in a white furnace: of %d pixels %d brighter than the open sky, %d much darker" % (
        "bitmask" if bitmask else "horizon", lit, brighter, darker))
    expect(brighter == 0 and darker < 0.12 * lit, "SSGI keeps a white furnace's energy")

# SSGI's tile order (hdvk:ssgiTileOrder): at half resolution each frame
# traces another pixel of every 2 x 2 tile, so the accumulated image
# differs from always tracing the top-left one; at full resolution there
# is nothing to rotate.
stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "cornell.usda"))


def tileOrderChanges(resolution):
    a, b = (render(stage, {"hdvk:taa": True, "hdvk:ssgi": True, "hdvk:ssgiResolution": resolution,
                           "hdvk:ssgiTileOrder": on}, frames=12) for on in (False, True))
    return sum(a.pixelColor(x, y) != b.pixelColor(x, y) for y in range(0, SIZE, 2) for x in range(0, SIZE, 2))


half, full = tileOrderChanges(2), tileOrderChanges(1)
print("SSGI tile order: %d pixels (of a quarter) change at half resolution, %d at full" % (half, full))
expect(half > 100 and full == 0, "hdvk:ssgiTileOrder rotates the traced pixel at half resolution only")

# Denoised SSR's noise through the sample dimensions (its GGX sample and
# kernel rotation, like every other effect's): a different pattern per
# frame than the gradient noise without them, the same converged film.
stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "reflections.usda"))
stage.GetPrimAtPath("/mtl/polished/pbr").GetAttribute("inputs:roughness").Set(0.25)
glossy = {**bright, "hdvk:ssrDenoise": True}
frames = [render(stage, {**glossy, "hdvk:sampleDimensions": on}) for on in (False, True)]
films = [render(stage, {**glossy, "hdvk:taa": True, "hdvk:sampleDimensions": on}, frames=70) for on in (False, True)]
floor = (20, 220, 380, 330)
changed = sum(frames[0].pixelColor(x, y) != frames[1].pixelColor(x, y)
              for y in range(floor[1], floor[3], 2) for x in range(floor[0], floor[2], 2))
converged = [mean(f, *floor) for f in films]
print("denoised SSR, sample dimensions: %d floor pixels (of a quarter) differ in a frame; films %.2f off, %.2f on" % (
    changed, converged[0], converged[1]))
expect(changed > 100 and abs(converged[0] - converged[1]) < 0.5,
       "denoised SSR's noise follows the sample dimensions to the same image")

# Back-face hits (hdvk:screenBackface): rays that meet what the camera sees
# from behind take hdvk:screenBackfaceScale of its light. SSGI's horizon scan
# in the Cornell box, where the floor around the tall box and the sphere
# sees their tops and backs, and SSR's chrome sphere over the floor, whose
# rays pass behind its silhouette: both darker with a scale of 0 than of 1,
# which is as without the option (SSR, horizon).
stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "cornell.usda"))
floor = (110, 230, 290, 270)
lit = {"hdvk:ssgi": True, "hdvk:exposureCompensation": 1.0}
one = mean(render(stage, {**lit, "hdvk:screenBackfaceScale": 1.0}, frames=8), *floor)
none = mean(render(stage, {**lit, "hdvk:screenBackfaceScale": 0.0}, frames=8), *floor)
off = mean(render(stage, {**lit, "hdvk:screenBackface": False}, frames=8), *floor)
print("SSGI back-face hits: the floor %.2f at scale 1, %.2f at 0, %.2f off" % (one, none, off))
expect(none < one - 0.2, "SSGI: back-face hits at scale 0 take no light")
expect(abs(one - off) < 0.05, "SSGI: back-face hits at scale 1 are as without the option")
stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "reflections.usda"))
sphere = (150, 80, 250, 240)  # the sphere and its reflection
one = mean(render(stage, {**bright, "hdvk:screenBackfaceScale": 1.0}), *sphere)
none = mean(render(stage, {**bright, "hdvk:screenBackfaceScale": 0.0}), *sphere)
off = mean(render(stage, {**bright, "hdvk:screenBackface": False}), *sphere)
print("SSR back-face hits: the sphere %.2f at scale 1, %.2f at 0, %.2f off" % (one, none, off))
expect(none < one - 0.1, "SSR: back-face hits at scale 0 take no light")
expect(abs(one - off) < 0.05, "SSR: back-face hits at scale 1 are as without the option")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
