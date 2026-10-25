"""Headless check of texture wrap modes (tests/scenes/wrap.usda) and several
uv sets per mesh (multiuv.usda): emission only and no lights or post
processing, so pixels are the textures' values.
Run: usdenv.cmd python tests\\test_textures.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

SIZE = 512

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()
failures = []


def render(scene):
    """The stage and an image of it through /cam, and the camera's matrix."""
    stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", scene))
    camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
    CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
    frustum = camera.frustum
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetRendererSetting("eldr:postProcess", False)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.clearColor = Gf.Vec4f(0, 0, 0, 1)
    for _ in range(2):  # textures arrive with the first commit
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    del engine  # release GL resources while the context is still current
    return image, frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()


def expect(image, viewProj, x, y, rgb, what):
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
red, green, blue, white = linear(230, 40, 40), linear(40, 200, 40), linear(40, 60, 255), (1, 1, 1)
black = (0, 0, 0)

# wrap.usda: quads 1.6 wide with st -0.5..1.5, so st = 0.5 + 1.25 * offset.
image, viewProj = render("wrap.usda")
cells = {  # centre, then what st (1.25, 0.25) and (-0.25, 1.25) read
    "UsdUVTexture repeat": ((-2.7, 2), red, green),
    "UsdUVTexture mirror": ((-0.9, 2), green, blue),
    "UsdUVTexture clamp": ((0.9, 2), green, blue),
    "UsdUVTexture black": ((2.7, 2), black, black),
    "MaterialX periodic": ((-2.7, 0), red, green),
    "MaterialX mirror": ((-0.9, 0), green, blue),
    "MaterialX clamp": ((0.9, 0), green, blue),
    "MaterialX constant": ((2.7, 0), black, black),
    "UsdUVTexture useMetadata (png: black)": ((-2.7, -2), black, black),
    "UsdUVTexture repeat, clamp": ((-0.9, -2), red, white),
    "MaterialX mirror, periodic": ((0.9, -2), green, red),
    "MaterialX graph clamp": ((2.7, -2), green, blue),
}
for what, ((cx, cy), right, topLeft) in cells.items():
    expect(image, viewProj, cx - 0.2, cy - 0.2, red, what + ", inside")
    expect(image, viewProj, cx + 0.6, cy - 0.2, right, what + ", right")
    expect(image, viewProj, cx - 0.6, cy + 0.6, topLeft, what + ", top left")

# multiuv.usda: quads 2 wide; st1 mirrors u, st2 mirrors v. At st (0.25,
# 0.25) st1 reads green and st2 blue; at st (0.75, 0.75) st1 blue, st2 green.
image, viewProj = render("multiuv.usda")
half = tuple((g + b) / 2 for g, b in zip(green, blue))
for what, x, low, high in (("UsdPrimvarReader st1", -2.5, green, blue), ("MaterialX texcoord index 1", 0, green, blue),
                           ("MaterialX geompropvalue st1 and st2", 2.5, half, half)):
    expect(image, viewProj, x - 0.5, -0.5, low, what + ", lower left")
    expect(image, viewProj, x + 0.5, 0.5, high, what + ", upper right")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
