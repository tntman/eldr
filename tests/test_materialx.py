"""Headless check of MaterialX graphs HdVk compiles into material programs:
tests/scenes/mtlxgraph.usda with every graph moved to emission (base and
lights off, no post processing), so pixels are the graphs' values.
Run: usdenv.cmd python tests\\test_materialx.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdShade

SIZE = 256

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "mtlxgraph.usda"))
stage.GetPrimAtPath("/sun").SetActive(False)
for prim in stage.Traverse():
    shader = UsdShade.Shader(prim)
    if shader and shader.GetIdAttr().Get() == "ND_standard_surface_surfaceshader":
        source = shader.GetInput("base_color").GetConnectedSources()[0][0]
        shader.CreateInput("base", Sdf.ValueTypeNames.Float).Set(0.0)
        shader.CreateInput("specular", Sdf.ValueTypeNames.Float).Set(0.0)
        shader.CreateInput("emission", Sdf.ValueTypeNames.Float).Set(1.0)
        shader.CreateInput("emission_color", Sdf.ValueTypeNames.Color3f).ConnectToSource(
            source.source, source.sourceName)

camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRendererSetting("hdvk:postProcess", False)
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
params.clearColor = Gf.Vec4f(0, 0, 0, 1)
for _ in range(2):  # textures arrive with the first commit
    engine.Render(stage.GetPseudoRoot(), params)
image = fbo.toImage()

failures = []


def expect(x, y, rgb, what):
    """The pixel at world (x, y, 0) is rgb (linear, 8-bit tolerance)."""
    p = viewProj.Transform(Gf.Vec3d(x, y, 0))
    px, py = int((p[0] * 0.5 + 0.5) * SIZE), int((0.5 - p[1] * 0.5) * SIZE)
    c = image.pixelColor(px, py)
    got = (c.redF(), c.greenF(), c.blueF())
    if any(abs(g - e) > 0.03 for g, e in zip(got, rgb)):
        failures.append(what)
        print(f"FAIL: {what}: expected {rgb}, got {tuple(round(g, 3) for g in got)}")


def linear(*srgb):
    return tuple(c / 255 / 12.92 if c <= 10 else ((c / 255 + 0.055) / 1.055) ** 2.4 for c in srgb)


# quadrants.png (sRGB): bottom red | green, top blue | white.
red, green = linear(230, 40, 40), linear(40, 200, 40)
expect(-3.0, 1.9, (red[0] * 0.25, red[1], red[2]), "multiply(texture, (0.25, 1, 1))")
expect(-2.0, 2.9, (0.25, 1, 1), "multiply, white quadrant")
expect(-0.5, 1.9, ((1 + red[0]) / 2, red[1] / 2, red[2] / 2), "mix(red, texture, 0.5)")
expect(0.5, 1.9, ((1 + green[0]) / 2, green[1] / 2, green[2] / 2), "mix(red, texture, 0.5), green quadrant")
expect(3.0, 1.9, green, "overlay(texture over 0.5 gray) is the texture")
expect(-2.5, 0, (0.1, 0.8, 0.1), "constant primvar")
# The centre is on the diagonal from the red corner to the blue one.
expect(0, 0, (0.5, 0, 0.5), "vertex primvar at the centre")
expect(2.5, 0, (0, 0.8, 0.8), "hsvadjust(red, hue + 0.5)")
expect(-3.2, -2.4, (1, 0.5, 0), "instance primvar, first")
expect(-0.4, -2.4, (0.6, 0, 1), "instance primvar, third")
expect(2.5, -2.4, (0, 2 / 3, 1 / 3), "Houdini ramp at 0.5")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
