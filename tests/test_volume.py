"""Headless check of HdVk's OpenVDB volumes (built against a USD with
OpenVDB): tests/scenes/volume.vdb's cloud loads (the render stats count
the volume and its grid memory), and fieldIndex 1 picks the file's second
"density" grid (of another size).
Run: usdenv.cmd python tests\\test_volume.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdShade, UsdVol

import hdvkUsdview

SIZE = 64
VDB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "volume.vdb")

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.CreateInMemory()
wall = UsdGeom.Mesh.Define(stage, "/wall")
wall.CreatePointsAttr([(-20, -20, -5), (20, -20, -5), (20, 20, -5), (-20, 20, -5)])
wall.CreateFaceVertexCountsAttr([4])
wall.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
material = UsdShade.Material.Define(stage, "/glow")
shader = UsdShade.Shader.Define(stage, "/glow/pbr")
shader.CreateIdAttr("UsdPreviewSurface")
shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set((0, 0, 0))
shader.CreateInput("emissiveColor", Sdf.ValueTypeNames.Color3f).Set((0.5, 0.5, 0.5))
material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
UsdShade.MaterialBindingAPI.Apply(wall.GetPrim()).Bind(material)

volume = UsdVol.Volume.Define(stage, "/cloud")
translate = volume.AddTranslateOp()
field = UsdVol.OpenVDBAsset.Define(stage, "/cloud/density")
field.CreateFilePathAttr(Sdf.AssetPath(VDB))
field.CreateFieldNameAttr("density")
volume.CreateFieldRelationship("density", field.GetPath())

camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 20))  # sees +-4.2
gfCamera = camera.GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(gfCamera, CameraUtil.Fit, 1.0)
frustum = gfCamera.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
engine.SetRendererSetting("hdvk:autoExposure", False)
params = UsdImagingGL.RenderParams()


def render():
    for _ in range(40):  # until TAA converges
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    return fbo.toImage()


failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


render()
stats = engine.GetRenderStats()
cloudBytes = stats.get("volumeMemory", 0)
expect(stats.get("volumes") == 1 and cloudBytes > 0, "stats count volumes and grid memory")
rows = dict(hdvkUsdview.Rows(stats))
expect("  volumes" in rows, "the usdview window shows the grid memory")

field.CreateFieldIndexAttr(1)  # the second "density": a ball of radius 0.5
render()
ballBytes = engine.GetRenderStats().get("volumeMemory", 0)
expect(0 < ballBytes != cloudBytes, "fieldIndex picks another grid")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
