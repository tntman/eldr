"""Displacement (tests/scenes/displacement.usda at Complexity High): the
settings switch it, the depth prepass sees the same displaced positions
as the shading pass, and the scale scales it.
Run: usdenv.cmd python tests\\test_displacement.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

SIZE = 256

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "displacement.usda"))
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum


def render(**settings):
    """An image with these hdvk: settings; noisy features off, so images compare."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for name, value in dict(taa=False, autoExposure=False, ambientOcclusion=False, ssr=False, **settings).items():
        engine.SetRendererSetting("hdvk:" + name, value)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.complexity = 1.2
    for _ in range(2):  # textures arrive with the first commit
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    del engine  # release GL resources while the context is still current
    return image


def difference(a, b):
    """Fraction of pixels whose colour differs by more than 4/255, and the largest difference."""
    differing, largest = 0, 0.0
    for y in range(SIZE):
        for x in range(SIZE):
            ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
            d = max(abs(ca.redF() - cb.redF()), abs(ca.greenF() - cb.greenF()), abs(ca.blueF() - cb.blueF()))
            differing += d > 4 / 255
            largest = max(largest, d)
    return differing / SIZE**2, largest


failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


on, off = render(), render(displacement=False)
changed, _ = difference(on, off)
expect(changed > 0.02, f"displacement changes the image ({changed:.3f} of pixels)")
# Without a prepass the shading pass writes its own depth: a prepass that
# saw other positions would leave holes or z-fighting in the other image.
changed, largest = difference(on, render(depthPrepass=False))
expect(changed < 0.002, f"the depth prepass sees the shading pass's positions ({changed:.4f} differ, {largest:.3f})")
changed, _ = difference(off, render(displacementScale=0.0))
expect(changed < 0.002, f"scale 0 is no displacement ({changed:.4f} differ)")
changed, _ = difference(on, render(displacementScale=2.0))
expect(changed > 0.02, f"the scale scales it ({changed:.3f} differ)")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
