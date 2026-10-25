"""Headless check of HdVk's screen-space effects through Hydra: the
thickness estimator (SSR at grazing angles), SSGI's visibility bitmask,
denoised glossy SSR, its footprint-filtered hits and back-face hits.
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
