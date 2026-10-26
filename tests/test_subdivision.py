"""Subdivision surfaces: tests/scenes/subdivision.usda refined with the
render params' complexity (usdview's Complexity menu), within the
eldr:subdivision and eldr:maxRefineLevel settings, or per mesh by
primvars:eldr:refineLevel and Karma's primvars:karma:object:dicingdepthmax.
Run: usdenv.cmd python tests\\test_subdivision.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL

SIZE = 64

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "subdivision.usda"))
frustum = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default()).frustum
engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()


def triangles(complexity):
    params.complexity = complexity
    engine.Render(stage.GetPseudoRoot(), params)
    return engine.GetRenderStats()["trianglesTotal"]


# Triangles per refine level: the ground (2, never refined), two quad cubes
# and a bilinear one (6 faces), a cube with a hole (5) and a loop
# octahedron (8 triangles); each level splits a quad into 4 quads (2
# triangles each) and a triangle into 4.
def expected(level):
    return 2 + 3 * 6 * 2 * 4**level + 5 * 2 * 4**level + 8 * 4**level


failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


for complexity, level in ((1.0, 0), (1.1, 1), (1.2, 2), (1.3, 3)):
    got = triangles(complexity)
    expect(got == expected(level), f"complexity {complexity}: {got} triangles, expected {expected(level)}")
expect(engine.GetRenderStats()["meshes"] == 7, "the smooth cube's GeomSubset is a mesh of its own")

# Settings re-sync the meshes without any scene change.
engine.SetRendererSetting("eldr:maxRefineLevel", 1)
expect(triangles(1.3) == expected(1), "maxRefineLevel clamps the level")
engine.SetRendererSetting("eldr:subdivision", False)
expect(triangles(1.3) == expected(0), "subdivision off draws the cages")
engine.SetRendererSetting("eldr:subdivision", True)
engine.SetRendererSetting("eldr:maxRefineLevel", 3)
expect(triangles(1.2) == expected(2), "back to the display style's level")

# Per mesh: the loop octahedron (8 triangles) at its own level, at most
# Karma's dicing depth.
loop = UsdGeom.PrimvarsAPI(stage.GetPrimAtPath("/loop"))
loop.CreatePrimvar("eldr:refineLevel", Sdf.ValueTypeNames.Int).Set(2)
expect(triangles(1.0) == expected(0) + 8 * (4**2 - 1), "primvars:eldr:refineLevel 2 over complexity's 0")
loop.CreatePrimvar("karma:object:dicingdepthmax", Sdf.ValueTypeNames.Int).Set(1)
expect(triangles(1.0) == expected(0) + 8 * (4 - 1), "primvars:karma:object:dicingdepthmax 1 caps it")
loop.RemovePrimvar("eldr:refineLevel")
expect(triangles(1.3) == expected(3) - 8 * (4**3 - 4), "the dicing depth caps the display style's level too")
loop.RemovePrimvar("karma:object:dicingdepthmax")
expect(triangles(1.3) == expected(3), "both removed")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
