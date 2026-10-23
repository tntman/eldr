"""Headless check of HdVk's BasisCurves and Points geometry: picks (HdVk's
primId AOV) where each curve basis must and must not reach.
Run: usdenv.cmd python tests\\test_curves.py"""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL, Vt

SIZE = 128

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.CreateInMemory()


def curve(name, points, curveType="cubic", basis="bspline", wrap="nonperiodic", width=0.4, normals=None):
    c = UsdGeom.BasisCurves.Define(stage, f"/{name}")
    c.CreateTypeAttr(curveType)
    c.CreateBasisAttr(basis)
    c.CreateWrapAttr(wrap)
    c.CreateCurveVertexCountsAttr([len(points)])
    c.CreatePointsAttr([Gf.Vec3f(x, y, 0) for x, y in points])
    c.CreateWidthsAttr([width])
    c.SetWidthsInterpolation(UsdGeom.Tokens.constant)
    if normals:
        c.CreateNormalsAttr([Gf.Vec3f(*normals)] * len(points))
        c.SetNormalsInterpolation(UsdGeom.Tokens.vertex)


hull = [(-2, 0), (-1, 0), (1, 0), (2, 0)]  # the cubic bases differ in how far they reach
curve("linear", [(-3, 3), (-1, 3)], curveType="linear")
curve("facing", [(1, 3), (3, 3)], curveType="linear", normals=(0, 0, 1))  # ribbon toward the camera
curve("edgeOn", [(1, 0.01), (3, 0.01)], curveType="linear", normals=(0, 1, 0))  # ribbon seen edge-on
curve("bspline", [(x, y + 1.5) for x, y in hull])
curve("catmullRom", [(x, y - 1) for x, y in hull], basis="catmullRom")
curve("pinned", [(x, y - 2) for x, y in hull], wrap="pinned")
curve("bezier", [(-3, -3.5), (-3, -2.5), (-1, -2.5), (-1, -3.5)], basis="bezier", width=0.2)
points = UsdGeom.Points.Define(stage, "/points")
points.CreatePointsAttr([Gf.Vec3f(2, -3, 0)])
points.CreateWidthsAttr([0.6])

camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 20))
gfCamera = camera.GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(gfCamera, CameraUtil.Fit, 1.0)
frustum = gfCamera.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
root = stage.GetPseudoRoot()
engine.Render(root, params)


def drawn(x, y):
    """The prim HdVk drew at world (x, y, 0)."""
    p = viewProj.Transform(Gf.Vec3d(x, y, 0))
    narrowed = frustum.ComputeNarrowedFrustum(Gf.Vec2d(p[0], p[1]), Gf.Vec2d(1.0 / SIZE, 1.0 / SIZE))
    pickParams = UsdImagingGL.Engine.PickParams()
    pickParams.resolveMode = "resolveNearestToCenter"
    hits = engine.TestIntersection(pickParams, narrowed.ComputeViewMatrix(),
                                   narrowed.ComputeProjectionMatrix(), root, params)
    return str(hits[0].hitPrimPath) if hits else None


failures = []


def expect(x, y, prim):
    hit = drawn(x, y)
    if hit != prim:
        failures.append((x, y, prim, hit))
        print(f"FAIL: at ({x}, {y}) expected {prim}, got {hit}")


expect(-2, 3, "/linear")
expect(-2, 3.35, None)            # radius 0.2
expect(2, 3, "/facing")
expect(2, 0, None)                # the edge-on ribbon has no area
expect(0, 1.5, "/bspline")
expect(1.2, 1.5, None)            # a bspline ends at (P1 + 4 P2 + P3) / 6, x = 5/6
expect(0.95, -1, "/catmullRom")   # catmullRom runs from P1 to P2
expect(1.3, -1, None)
expect(1.9, -2, "/pinned")        # pinned reaches the last point
expect(-2, -2.75, "/bezier")      # bezier middle: (P0 + 3 P1 + 3 P2 + P3) / 8
expect(-2, -2.5, None)
expect(2, -3, "/points")
expect(2.45, -3, None)            # radius 0.3

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
