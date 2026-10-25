"""usdview's viewport code path, without the window: the same UsdImagingGL
engine calls stageView.py makes (SetCameraState, Render into the GL
framebuffer, TestIntersection) in an offscreen GL context. Checks
  (a) picking at eldr:renderScale 1 and 0.5 finds the same prims at close depth,
  (b) the GL-shared colour and upscaled depth at scale 0.5 match scale 1,
  (c) a camera move followed by a still (TAA, then the film) has no jump in
      brightness; the frames are written as PNGs to HDVK_VIEWPORT_SEQ (default
      a temp folder),
  (d) the background pipeline prewarm on a cold cache keeps frames coming,
  (e) closing and reopening a stage 20 times with the prewarm on does not hang
      (faulthandler kills the process with all stacks after 240 s).
Run: usdenv.cmd python tests\\test_viewport.py"""
import faulthandler
import os
import shutil
import statistics
import sys
import tempfile
import time

import numpy as np
from OpenGL import GL
from PySide6.QtCore import QSize
from PySide6.QtGui import QImage, QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL

W, H = 320, 240
SCENE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "step4.usda")

faulthandler.dump_traceback_later(900, exit=True)  # the whole test; (e) has its own 240 s

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(W, H), QOpenGLFramebufferObject.CombinedDepthStencil)
fbo.bind()  # usdview's default framebuffer has colour and depth

cacheDir = tempfile.mkdtemp(prefix="eldr_viewport_cache_")
os.environ["HDVK_PIPELINE_CACHE_DIR"] = cacheDir  # nothing here touches the user's cache

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


class View:
    """A stage, an engine and a free camera, driven as stageView.py does."""

    def __init__(self, scale=1.0, prewarm=False, **settings):
        self.stage = Usd.Stage.Open(SCENE)
        self.root = self.stage.GetPseudoRoot()
        camera = UsdGeom.Camera(self.stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
        self.frustum = camera.frustum
        self.engine = UsdImagingGL.Engine()
        assert self.engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
        self.engine.SetRendererSetting("eldr:renderScale", scale)
        self.engine.SetRendererSetting("eldr:pipelinePrewarm", prewarm)
        for name, value in settings.items():
            self.engine.SetRendererSetting("eldr:" + name, value)
        self.params = UsdImagingGL.RenderParams()
        self.params.gammaCorrectColors = False
        self.engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
        self.dx = 0.0

    def Render(self, dx=None):
        """One viewport redraw with the camera dx to the side; seconds taken."""
        self.dx = self.dx if dx is None else dx
        view = self.frustum.ComputeViewMatrix() * Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(-self.dx, 0, 0))
        self.engine.SetCameraState(view, self.frustum.ComputeProjectionMatrix())
        GL.glClearColor(0, 0, 0, 0)
        GL.glEnable(GL.GL_DEPTH_TEST)
        GL.glDepthMask(GL.GL_TRUE)
        GL.glClear(GL.GL_COLOR_BUFFER_BIT | GL.GL_DEPTH_BUFFER_BIT)
        start = time.perf_counter()
        self.engine.Render(self.root, self.params)
        return time.perf_counter() - start

    def Pick(self, x, y):
        """(prim path, distance from the eye) under pixel (x, y), y down."""
        view = self.frustum.ComputeViewMatrix() * Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(-self.dx, 0, 0))
        frustum = Gf.Frustum(self.frustum)
        frustum.SetPositionAndRotationFromMatrix(view.GetInverse())
        ndc = Gf.Vec2d(2 * (x + 0.5) / W - 1, 1 - 2 * (y + 0.5) / H)
        narrowed = frustum.ComputeNarrowedFrustum(ndc, Gf.Vec2d(1.0 / W, 1.0 / H))
        pick = UsdImagingGL.Engine.PickParams()
        pick.resolveMode = "resolveNearestToCenter"  # as usdview
        hits = self.engine.TestIntersection(pick, narrowed.ComputeViewMatrix(),
                                            narrowed.ComputeProjectionMatrix(), self.root, self.params)
        if not hits or not hits[0].hitPrimPath:
            return None, None
        return str(hits[0].hitPrimPath), (hits[0].hitPoint - frustum.position).GetLength()

    @staticmethod
    def Colour():
        """The framebuffer's colour, float RGBA, top row first."""
        data = GL.glReadPixels(0, 0, W, H, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE)
        return np.frombuffer(data, np.uint8).reshape(H, W, 4)[::-1]

    @staticmethod
    def Depth():
        return np.asarray(GL.glReadPixels(0, 0, W, H, GL.GL_DEPTH_COMPONENT, GL.GL_FLOAT)).reshape(H, W)[::-1]

    def Close(self):
        del self.engine  # release GL resources while the context is still current
        self.stage = self.root = None


def Moved(view, frames=8, step=0.04):
    """The camera moves `frames` redraws: where an orbit in usdview is."""
    for i in range(1, frames + 1):
        view.Render(i * step)


# --- (a) picking and (b) GL sharing, scale 1 against 0.5, after a move ----
results = {}
for scale in (1.0, 0.5):
    view = View(scale)
    Moved(view)
    mode = view.engine.GetRenderStats().get("frameSharing")
    expect(mode == "OpenGL", "scale %g: colour and depth go to OpenGL (got %s)" % (scale, mode))
    pixels = [(x, y) for y in range(10, H, 25) for x in range(10, W, 30)]
    results[scale] = {"colour": view.Colour().astype(np.float32), "depth": view.Depth()}
    results[scale]["picks"] = [view.Pick(*p) for p in pixels]
    view.Close()

pickA, pickB = results[1.0]["picks"], results[0.5]["picks"]
same = [(a, b) for a, b in zip(pickA, pickB) if a[0] is not None and a[0] == b[0]]
hit = sum(a[0] is not None for a in pickA)
errs = sorted(abs(a[1] - b[1]) / a[1] for a, b in same) or [1]
depthErr, medianErr = errs[int(0.9 * len(errs))], errs[len(errs) // 2]
print(f"(a) picks: {hit} of {len(pickA)} pixels hit; {len(same)} same prim at both scales, "
      f"depth difference median {medianErr:.5f}, 90th percentile {depthErr:.5f}, worst {errs[-1]:.5f}")
expect(hit >= 10, "the pick grid hits the scene")
expect(len(same) >= 0.9 * hit, "picking finds the same prim at scale 0.5 as at 1")
expect(medianErr < 0.01 and depthErr < 0.03, "pick depth at scale 0.5 close to scale 1")

c1, c2 = results[1.0]["colour"], results[0.5]["colour"]
d1, d2 = results[1.0]["depth"], results[0.5]["depth"]
colourDiff = float(np.abs(c1[..., :3] - c2[..., :3]).mean())
cover1, cover2 = d1 < 1, d2 < 1
overlap = float((cover1 & cover2).sum() / max((cover1 | cover2).sum(), 1))
both = cover1 & cover2
depthDiff = float(np.abs(d1[both] - d2[both]).mean()) if both.any() else 1.0
print(f"(b) GL sharing: colour mean abs difference {colourDiff:.2f}/255 (scale 1 mean {c1[..., :3].mean():.1f}), "
      f"depth coverage overlap {overlap:.3f}, mean depth difference {depthDiff:.5f}")
expect(c2[..., :3].mean() > 5 and cover2.mean() > 0.2, "the 0.5 frame shows the scene in colour and depth")
expect(colourDiff < 6, "colour at scale 0.5 close to scale 1")
expect(overlap > 0.95, "upscaled depth covers the same pixels as full-size depth")
expect(depthDiff < 0.003, "upscaled depth close to full-size depth")

# --- (c) TAA to film -------------------------------------------------------
seq = os.environ.get("HDVK_VIEWPORT_SEQ") or tempfile.mkdtemp(prefix="eldr_viewport_seq_")
os.makedirs(seq, exist_ok=True)
for scale in (1.0, 0.5):
    view = View(scale)
    means = []
    for i in range(1, 13):                         # moving: TAA
        view.Render(i * 0.04)
        means.append(float(view.Colour()[..., :3].mean()))
        QImage(np.ascontiguousarray(view.Colour()).data, W, H, 4 * W, QImage.Format_RGBA8888).save(
            os.path.join(seq, "scale%g_%03d_move.png" % (scale, len(means))))
    moved = len(means)
    for _ in range(150):                           # still: TAA for 3 frames, then the film
        view.Render()
        means.append(float(view.Colour()[..., :3].mean()))
        QImage(np.ascontiguousarray(view.Colour()).data, W, H, 4 * W, QImage.Format_RGBA8888).save(
            os.path.join(seq, "scale%g_%03d_still.png" % (scale, len(means))))
        if view.engine.IsConverged():
            break
    converged = view.engine.IsConverged()
    jumps = [abs(means[i] - means[i - 1]) for i in range(moved, len(means))]
    worst = max(jumps) / means[-1]
    print(f"(c) scale {scale:g}: {moved} moving + {len(means) - moved} still frames, converged {converged}, "
          f"mean {means[moved - 1]:.2f} -> {means[-1]:.2f}, worst frame-to-frame step after the move {100 * worst:.2f}%")
    expect(converged, "scale %g: the still camera converges" % scale)
    expect(worst < 0.03, "scale %g: no brightness jump above 3%% at the TAA to film switch" % scale)
    view.Close()
print("    frames written to", seq)

# --- (d) the prewarm on a cold cache ---------------------------------------
for prewarm in (False, True):
    shutil.rmtree(cacheDir, ignore_errors=True)    # cold: every shading set compiles
    view = View(1.0, prewarm)
    cpu0 = time.process_time()
    wall0 = time.perf_counter()
    times = []
    for i in range(30):
        times.append(view.Render(0.04 * (i % 4)))   # a short back-and-forth: orbiting
        # a viewer's event loop idles between redraws; the worker gets that time
        time.sleep(0.005)
    cpu = time.process_time() - cpu0
    wall = time.perf_counter() - wall0
    rest = times[1:]
    print(f"(d) prewarm {'on ' if prewarm else 'off'}: first frame {times[0]:.2f} s, then median {1000 * statistics.median(rest):.0f} ms, "
          f"max {1000 * max(rest):.0f} ms; CPU {cpu:.1f} s over {wall:.1f} s wall")
    if prewarm:
        expect(max(rest) < 0.75, "prewarm: no frame after the first stalls above 750 ms")
        expect(statistics.median(rest) < 0.1, "prewarm: median frame under 100 ms")
    view.Close()

# --- (e) close and reopen --------------------------------------------------
faulthandler.dump_traceback_later(240, exit=True)  # the watchdog: a hang prints every thread's stack and exits
start = time.perf_counter()
for i in range(20):
    view = View(0.5 if i % 2 else 1.0, True, gi=bool(i % 3 == 0))  # a toggled feature queues more prewarm sets
    for f in range(2 + i % 3):
        view.Render(0.03 * f)
    view.Close()                                   # closed with the worker likely still compiling
print(f"(e) 20 reopen cycles with the prewarm on: {time.perf_counter() - start:.1f} s")
faulthandler.cancel_dump_traceback_later()
faulthandler.dump_traceback_later(900, exit=True)

shutil.rmtree(cacheDir, ignore_errors=True)
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
