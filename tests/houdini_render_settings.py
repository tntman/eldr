r"""hython tests\houdini_render_settings.py <build-h22>: a default Render Settings
LOP over step4.usda changes nothing in Eldr's Solaris viewport. Its camera
relationship names /cameras/camera1, which doesn't exist: looking through it
keeps the viewport's camera, the frame stays interactive (houdini:viewport),
and the image matches the one without the settings prim.
Run with HOUDINI_PACKAGE_DIR=<build-h22>\houdini and HOUDINI_USER_PREF_DIR set
to an empty folder ending in houdini__HVER__; see docs/houdini.md."""
import os
import sys

import hou
from hutil.PySide.QtCore import QSize
from hutil.PySide.QtGui import QOffscreenSurface, QOpenGLContext
from hutil.PySide.QtOpenGL import QOpenGLFramebufferObject
from hutil.PySide.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL

W, H = 320, 180
app = QApplication.instance() or QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(W, H))
fbo.bind()

net = hou.node("/stage")
sub = net.createNode("sublayer")
sub.parm("filepath1").set(os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "step4.usda"))
rs = net.createNode("rendersettings")
rs.setInput(0, sub)
stage = rs.stage()
settings = stage.GetPrimAtPath("/Render/rendersettings")
failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


# The LOP's Eldr tab is all Do Nothing: no eldr:, husk: or karma: attributes.
authored = [a.GetName() for a in settings.GetAuthoredAttributes() if ":" in a.GetName()]
expect(not authored, "a default Render Settings LOP authors no renderer attributes: %s" % authored)
expect(not stage.GetPrimAtPath(settings.GetRelationship("camera").GetTargets()[0]),
       "the default camera is missing (the case under test)")

camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, W / H)
frustum = camera.frustum


def render(look_through):
    """The viewport: a free camera, then looking through the settings prim
    (active, its missing camera). Returns the image and the stages seen."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "Eldr not available"
    engine.SetRendererSetting("houdini:viewport", True)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    stages = set()
    for i in range(80):
        if look_through and i == 2:
            engine.SetActiveRenderSettingsPrimPath(settings.GetPath())
            engine.SetCameraPath(Sdf.Path("/cameras/camera1"))
        engine.Render(stage.GetPseudoRoot(), params)
        stages.add(engine.GetRenderStats().get("rendererStage"))
    image = fbo.toImage()
    del engine
    return image, stages


plain, plainStages = render(False)
looked, lookedStages = render(True)
diff = sum(abs(a - b) for y in range(0, H, 4) for x in range(0, W, 4)
           for a, b in zip(plain.pixelColor(x, y).getRgb()[:3], looked.pixelColor(x, y).getRgb()[:3]))
mean = diff / (3 * (W // 4) * (H // 4))
print("mean difference %.2f (8-bit), stages %s / %s" % (mean, sorted(plainStages), sorted(lookedStages)))
expect(mean < 2.0, "the same image with and without the settings prim (mean difference %.2f)" % mean)
expect("final" not in plainStages | lookedStages, "the viewport's film is never final")
if failures:
    sys.exit(1)
print("OK")
