"""Crowd instancing (HD_ENABLE_DEFERRED_SKINNING=1): a Houdini-style crowd (make_crowd.py
"houdini": an animated Xform per agent above an instanceable SkelRoot whose
mesh maps its skel:joints) renders as one instanced mesh per prototype, the
same image as with every agent its own mesh. HD_ENABLE_DEFERRED_SKINNING is
read once per process, so each mode renders in a child with it set.
Run: usdenv.cmd python tests\\test_crowd_instancing.py"""
import os
import subprocess
import sys
import tempfile

W, H, FRAME = 192, 108, 12


def child(scene, out):
    from PySide6.QtCore import QSize
    from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
    from PySide6.QtOpenGL import QOpenGLFramebufferObject
    from PySide6.QtWidgets import QApplication
    from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

    app = QApplication(sys.argv)
    surface = QOffscreenSurface()
    surface.create()
    glContext = QOpenGLContext()  # must stay referenced while rendering
    glContext.create()
    glContext.makeCurrent(surface)
    fbo = QOpenGLFramebufferObject(QSize(W, H))
    fbo.bind()
    stage = Usd.Stage.Open(scene)
    camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(FRAME)
    CameraUtil.ConformWindow(camera, CameraUtil.Fit, W / H)
    frustum = camera.frustum
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.frame = Usd.TimeCode(FRAME)
    for _ in range(400):  # until the film converges
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    print("meshes", engine.GetRenderStats().get("meshes"))
    fbo.toImage().save(out)
    del engine


def render(scene, out, deferred):
    env = dict(os.environ)
    # UsdImagingGL.Engine starts with Storm, which reads the setting before
    # Eldr's delegate could turn it on: set it for the process.
    env["HD_ENABLE_DEFERRED_SKINNING"] = "1" if deferred else "0"
    p = subprocess.run([sys.executable, __file__, "--child", scene, out], env=env, capture_output=True, text=True,
                       timeout=600)
    sys.stderr.write(p.stderr)
    meshes = [int(line.split()[1]) for line in p.stdout.splitlines() if line.startswith("meshes ")]
    return p.returncode == 0 and meshes and meshes[0]


def main():
    import numpy as np
    from PySide6.QtGui import QImage

    folder = tempfile.mkdtemp(prefix="eldr_crowd_")
    scene = os.path.join(folder, "crowd.usda")
    subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes",
                                                 "make_crowd.py"), scene, "50", "houdini"], check=True)
    failures = []

    def expect(ok, what):
        if not ok:
            failures.append(what)
            print("FAIL:", what, file=sys.stderr)

    perAgent = render(scene, os.path.join(folder, "off.png"), False)
    shared = render(scene, os.path.join(folder, "on.png"), True)
    print("meshes: %s per agent, %s instanced" % (perAgent, shared))
    expect(perAgent and perAgent >= 50, "every agent its own mesh without crowd instancing: %s" % perAgent)
    expect(shared and shared < 10, "one mesh for all agents with crowd instancing: %s" % shared)

    def pixels(name):
        img = QImage(os.path.join(folder, name)).convertToFormat(QImage.Format_RGBA8888)
        return np.frombuffer(img.constBits(), np.uint8).reshape(img.height(), img.width(), 4)[..., :3].astype(float)

    if perAgent and shared:
        a, b = pixels("off.png"), pixels("on.png")
        diff = np.abs(a - b)
        lit = (a.sum(axis=2) > 0).mean()
        print("mean diff %.3f, max %d, pixels off by > 8: %.4f, lit %.2f" %
              (diff.mean(), diff.max(), (diff.max(axis=2) > 8).mean(), lit))
        expect(lit > 0.2, "the crowd is in the picture")
        expect(diff.mean() < 0.5 and (diff.max(axis=2) > 8).mean() < 0.002, "same image either way")
    print("FAILED" if failures else "OK")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        child(sys.argv[2], sys.argv[3])
    else:
        main()
