"""Lens distortion through Hydra: a USD camera's lensDistortion:* custom
attributes reach HdCamera and warp the image; a Karma lens material with
the same terms warps it the same; eldr:lensDistortion off renders the
plain camera's view.
Run: usdenv.cmd python tests\\test_distortion.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdImagingGL

SIZE = 64

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "distortion.usda"))


def Render(camera, distortion=True):
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetRendererSetting("eldr:taa", False)
    engine.SetRendererSetting("eldr:lensDistortion", distortion)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraPath(Sdf.Path(camera))
    params = UsdImagingGL.RenderParams()
    for _ in range(2):
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    passes = engine.GetRenderStats()["gpuPassMs"]
    del engine
    return [image.pixel(x, y) for y in range(SIZE) for x in range(SIZE)], passes


plain, plainPasses = Render("/plain")
barrel, barrelPasses = Render("/cam")
off, _ = Render("/cam", distortion=False)
karma, karmaPasses = Render("/karma")

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


expect("lensDistortion" in barrelPasses and "lensDistortion" not in plainPasses, "the warp runs for /cam only")
expect(sum(a != b for a, b in zip(plain, barrel)) > SIZE * SIZE // 10, "the barrel lens changes the image")
expect("lensDistortion" in karmaPasses, "Karma's lens material distorts")
# Within a step of 8 bits: exposure adapts by a hair between runs.
same = all(abs(((a >> k) & 255) - ((b >> k) & 255)) <= 1 for a, b in zip(plain, off) for k in (0, 8, 16))
expect(same, "eldr:lensDistortion off: the plain view")
expect(all(abs(((a >> k) & 255) - ((b >> k) & 255)) <= 1 for a, b in zip(barrel, karma) for k in (0, 8, 16)),
       "Karma's lens material: the same warp as lensDistortion:k1, k2")

solaris, _ = Render("/solaris")
expect(all(abs(((a >> k) & 255) - ((b >> k) & 255)) <= 1 for a, b in zip(barrel, solaris) for k in (0, 8, 16)),
       "Solaris's lens string (karma:camera:lensshader): the same warp as lensDistortion:k1, k2")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
