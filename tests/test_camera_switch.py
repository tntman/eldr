"""Houdini's viewport dropping out of a scene camera: with eldr:motionBlur on
(the film's time samples), frames through a scene camera whose shutter is
open, then Houdini's free camera (/__HoudiniFreeCamera__, shutter -0.25 to
0.25 as its FreeCamera.usda) moves a few redraws and stops. Its converged
film must match a view that looked through the free camera from the start:
no camera blur from the scene camera's pose (the whole image smeared), while
the moving cube still blurs by its own motion.
Run: usdenv.cmd python tests\\test_camera_switch.py
     [--scene file --camera /path --time t --out folder] (any scene, e.g. a
     crowd with HD_ENABLE_DEFERRED_SKINNING=1; images saved to --out)"""
import argparse
import os
import sys
import tempfile

import numpy as np
from OpenGL import GL
from PySide6.QtCore import QSize
from PySide6.QtGui import QImage, QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux

parser = argparse.ArgumentParser()
parser.add_argument("--scene")
parser.add_argument("--camera", default="/cam")
parser.add_argument("--time", type=float, default=2.0)
parser.add_argument("--out")
parser.add_argument("--size", default="128x128")
args = parser.parse_args()
W, H = (int(v) for v in args.size.split("x"))

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(W, H), QOpenGLFramebufferObject.CombinedDepthStencil)
fbo.bind()

from pxr import UsdImagingGL  # noqa: E402  (after the GL context)

failures = []


def expect(ok, what):
    print("ok:" if ok else "FAIL:", what, file=sys.stdout if ok else sys.stderr)
    if not ok:
        failures.append(what)


def square(camera):
    camera.CreateFocalLengthAttr(35.0)
    camera.CreateHorizontalApertureAttr(20.0)
    camera.CreateVerticalApertureAttr(20.0 * H / W)


if args.scene:
    stage = Usd.Stage.Open(args.scene)
else:
    # A red cube moving 2 units a frame along x in front of a static wall
    # of grey and white blocks (whose edges show any camera smear).
    stage = Usd.Stage.CreateInMemory()
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    stage.SetStartTimeCode(1)
    stage.SetEndTimeCode(3)
    cube = UsdGeom.Cube.Define(stage, "/cube")
    cube.CreateSizeAttr(1.0)
    cube.CreateDisplayColorAttr([Gf.Vec3f(1, 0.1, 0.1)])
    move = cube.AddTranslateOp()
    move.Set(Gf.Vec3d(-2, 0, 0), 1)
    move.Set(Gf.Vec3d(2, 0, 0), 3)
    for i in range(10):
        for j in range(10):
            block = UsdGeom.Cube.Define(stage, "/wall/b%d_%d" % (i, j))
            block.CreateSizeAttr(1.0)
            grey = 0.9 if (i + j) % 2 else 0.2
            block.CreateDisplayColorAttr([Gf.Vec3f(grey, grey, grey)])
            block.AddTranslateOp().Set(Gf.Vec3d(i - 4.5, j - 4.5, -4))
    sun = UsdLux.DistantLight.Define(stage, "/sun")
    sun.CreateIntensityAttr(3.0)
    sun.CreateNormalizeAttr(True)
    cam = UsdGeom.Camera.Define(stage, "/cam")
    cam.AddTranslateOp().Set(Gf.Vec3d(0, 0, 10))
    square(cam)

stage.SetEditTarget(stage.GetSessionLayer())
scene = UsdGeom.Camera(stage.GetPrimAtPath(args.camera))
scene.GetShutterOpenAttr().Set(-0.25)
scene.GetShutterCloseAttr().Set(0.25)
shot = scene.GetCamera(Usd.TimeCode(args.time))
FREE = Sdf.Path("/__HoudiniFreeCamera__")
free = UsdGeom.Camera.Define(stage, FREE)
PAN = 5  # redraws the free camera moves


def place(step):
    """The free camera: the scene camera's, turned `step` redraws to the side."""
    camera = Gf.Camera(shot)
    side = Gf.Matrix4d(1).SetRotate(Gf.Rotation(Gf.Vec3d(0, 1, 0), 1.0 * min(step, PAN)))
    camera.transform = side * shot.transform
    free.SetFromCamera(camera, Usd.TimeCode.Default())
    free.GetShutterOpenAttr().Set(-0.25)
    free.GetShutterCloseAttr().Set(0.25)


def colour():
    data = GL.glReadPixels(0, 0, W, H, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE)
    return np.frombuffer(data, np.uint8).reshape(H, W, 4)[::-1, :, :3].astype(np.float32)


def run(switch, motionBlur=True, sampled=True):
    """The free camera's converged film; `switch`: after frames through the scene camera."""
    place(0 if switch else PAN)
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for key, value in {"houdini:viewport": True, "eldr:autoExposure": False, "eldr:screenDicing": False,
                       "eldr:motionBlur": motionBlur, "eldr:sampledMotion": sampled}.items():
        engine.SetRendererSetting(key, value)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
    params = UsdImagingGL.RenderParams()
    params.frame = Usd.TimeCode(args.time)
    params.gammaCorrectColors = False
    params.showRender = True  # crowds exported with purpose render (Houdini draws them)
    root = stage.GetPseudoRoot()

    def render():
        GL.glClearColor(0, 0, 0, 0)
        GL.glClear(GL.GL_COLOR_BUFFER_BIT | GL.GL_DEPTH_BUFFER_BIT)
        engine.Render(root, params)

    if switch:
        engine.SetCameraPath(scene.GetPath())
        for _ in range(40):
            render()
        before = colour()
        engine.SetCameraPath(FREE)  # Houdini edits its camera prim each redraw
        for step in range(1, PAN + 1):
            place(step)
            render()
    else:
        engine.SetCameraPath(FREE)
    for frames in range(1, 600):
        render()
        if engine.IsConverged():
            break
    image = colour()
    del engine  # GL resources go while the context is current
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        for name, pixels in ([("before", before)] if switch else []) + [
                ("switched" if switch else "reference" if motionBlur else "sharp", image)]:
            QImage(np.ascontiguousarray(pixels.astype(np.uint8)).data, W, H, 3 * W,
                   QImage.Format_RGB888).copy().save(os.path.join(args.out, name + ".png"))
    print("%s: converged in %d frames" % ("switched" if switch else "reference", frames))
    return image


# Sampled motion (the default) and the motion from the frame before.
for sampled in (True, False):
    reference = run(False, sampled=sampled)
    switched = run(True, sampled=sampled)
    sharp = run(False, motionBlur=False)
    diff = float(np.abs(switched - reference).mean())
    blur = float(np.abs(reference - sharp).mean())
    expect(diff < 2.0, "sampled motion %s: the free camera after a scene camera matches it from the start "
           "(mean difference %.3f of 255; motion blur vs none %.3f)" % (sampled, diff, blur))
    if sampled and not args.scene:
        expect(blur > 0.5, "the moving cube blurs (mean difference to motion blur off %.3f of 255)" % blur)
if failures:
    sys.exit(1)
print("OK")