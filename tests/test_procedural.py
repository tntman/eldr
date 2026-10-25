"""MaterialX procedurals (noise, fractal, cell and worley noise, position,
normal) against Storm, which renders MaterialX's own GLSL:
tests/scenes/procedural.usda, emission only, no lights or post processing,
so pixels are the graphs' values in both.
Run: usdenv.cmd python tests\\test_procedural.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

SIZE = 768

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "procedural.usda"))
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()


def render(plugin, settings):
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin(plugin), plugin + " not available"
    for key, value in settings.items():
        engine.SetRendererSetting(key, value)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.clearColor = Gf.Vec4f(0, 0, 0, 1)
    for _ in range(3):  # Storm compiles its shaders, HdVk commits textures
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    del engine  # release GL resources while the context is still current
    return image


def pixels(image, x, y):
    """Colours over the quad centred at world (x, y), 24 x 24 samples
    inside its edges."""
    out = []
    for i in range(24):
        for j in range(24):
            p = viewProj.Transform(Gf.Vec3d(x - 0.75 + 1.5 * i / 23, y - 0.75 + 1.5 * j / 23, 0))
            c = image.pixelColor(int((p[0] * 0.5 + 0.5) * SIZE), int((0.5 - p[1] * 0.5) * SIZE))
            out.append((c.redF(), c.greenF(), c.blueF()))
    return out


eldr = render("HdVkRendererPlugin", {"eldr:postProcess": False})
storm = render("HdStormRendererPlugin", {})
failures = []
quads = {"noise2d": (-3.6, 2.4), "noise3d (object space, transformed quad)": (-1.2, 2.4),
         "fractal3d": (1.2, 2.4), "cellnoise2d": (3.6, 2.4), "cellnoise3d": (-3.6, 0), "worleynoise2d": (-1.2, 0),
         "worleynoise3d solid": (1.2, 0), "worleynoise2d vector2": (3.6, 0), "world position": (-1.2, -2.4),
         "object normal": (1.2, -2.4), "noise3d vector4, world space": (3.6, -2.4)}
for what, (x, y) in quads.items():
    a, b = pixels(eldr, x, y), pixels(storm, x, y)
    # Cell edges may fall on either side of a sample: a few may differ.
    off = sum(any(abs(p - q) > 0.03 for p, q in zip(pa, pb)) for pa, pb in zip(a, b))
    spread = max(max(c) for c in a) - min(min(c) for c in a)
    mean = sum(abs(p - q) for pa, pb in zip(a, b) for p, q in zip(pa, pb)) / (3 * len(a))
    print(f"{what}: {off} of {len(a)} samples differ, mean difference {mean:.4f}, range {spread:.2f}")
    if off > len(a) // 50:
        failures.append(what)
        worst = max(zip(a, b), key=lambda ab: max(abs(p - q) for p, q in zip(*ab)))
        print(f"FAIL: {what}: {off} of {len(a)} samples differ from Storm, e.g. {worst}")
    if what != "object normal" and spread < 0.1:
        failures.append(what)
        print(f"FAIL: {what}: flat ({spread:.3f})")

# noise2d on the default texture coordinates reads st (Storm reads 0 there):
# the same noise as on st itself, at a sixth of noise2d's frequency.
flat = pixels(eldr, -3.6, -2.4)
if max(c[0] for c in flat) - min(c[0] for c in flat) < 0.05:
    failures.append("noise2d on default texcoords")
    print("FAIL: noise2d on default texture coordinates is flat")

# Procedural Textures off: the noise reads 0, so noise2d is its pivot.
off = render("HdVkRendererPlugin", {"eldr:postProcess": False, "eldr:proceduralTextures": False})
if any(abs(c - 0.5) > 0.01 for p in pixels(off, -3.6, 2.4) for c in p):
    failures.append("Procedural Textures off")
    print("FAIL: Procedural Textures off: noise2d isn't its pivot 0.5")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
