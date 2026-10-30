"""eldr:denoise: a final film of the emissive Cornell box (GI, final gather) at 4 samples (box pixel filter, as the denoiser uses), denoised once
converged, is closer to a 256-sample reference than the raw 4 samples; the
denoise ran (denoiseMs); the eldr:beautyRaw AOV holds the raw image.
Skips (passes) when Eldr was built without Intel Open Image Denoise.
Run: usdenv.cmd python tests\\test_denoise.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL

W, H = 320, 240
app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(W, H))
fbo.bind()
stage = Usd.Stage.Open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "emissive.usda"))
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
camera.SetPerspectiveFromAspectRatioAndFieldOfView(W / H, camera.GetFieldOfView(Gf.Camera.FOVVertical),
                                                   Gf.Camera.FOVVertical)
frustum = camera.frustum
failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def Render(samples, denoise, aov="color"):
    """Pixels (r, g, b floats) of a converged final film, and the render stats."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for key, value in {"eldr:finalQuality": True, "eldr:filmSamples": samples, "eldr:denoise": denoise, "eldr:gi": True, "eldr:filmFilterRadius": 0.0,
                       "eldr:finalGather": 1}.items():
        engine.SetRendererSetting(key, value)
    engine.SetRendererAov(aov)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    for _ in range(samples + 500):  # GI probes settle first (eldr:giPreconverge)
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    image = fbo.toImage()
    pixels = [image.pixelColor(x, y) for y in range(H) for x in range(W)]
    stats = engine.GetRenderStats()
    del engine
    return [(c.redF(), c.greenF(), c.blueF()) for c in pixels], stats


def Rms(a, b):
    return (sum((x - y) ** 2 for p, q in zip(a, b) for x, y in zip(p, q)) / (3 * len(a))) ** 0.5


reference, _ = Render(256, False)
raw, _ = Render(4, False)
denoised, stats = Render(4, True)
if stats.get("denoiseMs", 0.0) <= 0.0 and Rms(raw, denoised) == 0.0:
    print("SKIP: no denoising (built without OIDN?)")
    sys.exit(0)
rawError, denoisedError = Rms(raw, reference), Rms(denoised, reference)
print("4 samples vs 256: raw RMS %.4f, denoised %.4f, raw vs denoised %.4f (%.0f ms)" % (rawError, denoisedError, Rms(raw, denoised), stats["denoiseMs"]))
expect(stats["denoiseMs"] > 0.0, "the denoise ran (denoiseMs)")
expect(denoisedError < 0.8 * rawError, "denoised 4 samples closer to the reference than raw ones")
# The raw beauty AOV, linear (usdview shows it unmapped): lit, and not the denoised image.
rawAov, _ = Render(4, True, "eldr:beautyRaw")
expect(max(max(p) for p in rawAov) > 0.05, "eldr:beautyRaw holds an image")
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
