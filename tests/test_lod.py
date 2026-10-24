"""Levels of detail: 400 instances of a 3968-triangle sphere receding from the
camera draw far fewer triangles with hdvk:lod on, look the same, and the
bias trades triangles for error.
Run: usdenv.cmd python tests\\test_lod.py"""
import math
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL, UsdLux, Vt

SIZE = 256

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

# A UV sphere of 64 x 32 segments, no uvs or normals.
stage = Usd.Stage.CreateInMemory()
sphere = UsdGeom.Mesh.Define(stage, "/proto/sphere")
rings, segments = 32, 64
points = [Gf.Vec3f(0, 1, 0)]
for r in range(1, rings):
    theta = math.pi * r / rings
    for s in range(segments):
        phi = 2 * math.pi * s / segments
        points.append(Gf.Vec3f(math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi)))
points.append(Gf.Vec3f(0, -1, 0))
counts, indices = [], []
ring = lambda r, s: 1 + (r - 1) * segments + s % segments
for s in range(segments):
    counts.append(3)
    indices += [0, ring(1, s + 1), ring(1, s)]
    counts.append(3)
    indices += [len(points) - 1, ring(rings - 1, s), ring(rings - 1, s + 1)]
for r in range(1, rings - 1):
    for s in range(segments):
        counts.append(4)
        indices += [ring(r, s), ring(r, s + 1), ring(r + 1, s + 1), ring(r + 1, s)]
sphere.CreatePointsAttr(points)
sphere.CreateFaceVertexCountsAttr(counts)
sphere.CreateFaceVertexIndicesAttr(indices)
sphere.CreateSubdivisionSchemeAttr("none")
instancer = UsdGeom.PointInstancer.Define(stage, "/crowd")
instancer.CreatePrototypesRel().SetTargets(["/proto/sphere"])
positions = [Gf.Vec3f((i % 20 - 9.5) * 3, 0, -3 - (i // 20) * 6) for i in range(400)]
instancer.CreatePositionsAttr(Vt.Vec3fArray(positions))
instancer.CreateProtoIndicesAttr([0] * len(positions))
UsdGeom.Imageable(stage.GetPrimAtPath("/proto")).MakeInvisible()
UsdLux.DistantLight.Define(stage, "/sun")
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 4, 6))
camera.AddRotateXOp().Set(-15)
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum


def render(**settings):
    """Stats and an image once converged (levels of detail build meanwhile)."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for name, value in dict(taa=False, autoExposure=False, **settings).items():
        engine.SetRendererSetting("hdvk:" + name, value)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    for _ in range(200):
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    stats, image = engine.GetRenderStats(), fbo.toImage()
    del engine  # release GL resources while the context is still current
    return stats, image


def difference(a, b):
    """Fraction of pixels whose colour differs by more than 16/255."""
    differing = 0
    for y in range(SIZE):
        for x in range(SIZE):
            ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
            differing += max(abs(ca.red() - cb.red()), abs(ca.green() - cb.green()), abs(ca.blue() - cb.blue())) > 16
    return differing / SIZE**2


failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


full, fullImage = render(lod=False)
lod, lodImage = render()
coarse, _ = render(lodBias=3.0)
drawn = lambda s: s["trianglesDrawn"]
triangles = sum(c - 2 for c in counts)
expect(drawn(full) == full["instancesDrawn"] * triangles, f"lod off draws every triangle ({drawn(full)})")
expect(drawn(lod) < 0.6 * drawn(full), f"levels of detail draw fewer triangles ({drawn(lod)} of {drawn(full)})")
expect(drawn(coarse) < drawn(lod), f"a bias of 3 draws fewer still ({drawn(coarse)})")
expect(lod["instancesDrawn"] == full["instancesDrawn"], "the same instances")
changed = difference(fullImage, lodImage)
expect(changed < 0.01, f"and they look the same ({changed:.4f} of pixels differ)")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
