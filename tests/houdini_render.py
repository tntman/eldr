"""Renders one frame with Eldr inside hython (Houdini's USD and PySide6) and
saves it as a PNG. Used by docs/houdini.md; not part of ctest.
Run: hython tests\\houdini_render.py <scene.usda> <camera prim> <out.png>
with PXR_PLUGINPATH_NAME=<build-h22 dir>\\ set."""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

scene, cameraPath, out = sys.argv[1:4]
SIZE = 256
app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(scene)
camera = UsdGeom.Camera(stage.GetPrimAtPath(cameraPath)).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum
engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "Eldr not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
for _ in range(100):
    engine.Render(stage.GetPseudoRoot(), params)
    if engine.IsConverged():
        break
image = fbo.toImage()
print("centre pixel", image.pixelColor(SIZE // 2, SIZE // 2).getRgb(), "saved", image.save(out))
del engine  # before the GL context goes
