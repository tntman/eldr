"""Holdouts through Hydra: gprims with primvars:ri:attributes:Ri:Matte
(hdPrman's), primvars:karma:object:holdoutmode (Karma's) or in the active
UsdRenderPass's matte collection render black with zero alpha over a red
wall; hdvk:holdouts off draws them as usual.
Run: usdenv.cmd python tests\\test_holdout.py"""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdRender, UsdShade

SIZE = 64

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.CreateInMemory()


def emitter(path, rgb):
    mat = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/pbr")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set((0, 0, 0))
    shader.CreateInput("emissiveColor", Sdf.ValueTypeNames.Color3f).Set(rgb)
    mat.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    return mat


def quad(path, x0, y0, x1, y1, z, mat):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateSubdivisionSchemeAttr("none")
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)
    return mesh


red, green = emitter("/mtl/red", (1, 0, 0)), emitter("/mtl/green", (0, 1, 0))
quad("/wall", -2, -2, 2, 2, -1, red)
ri = quad("/ri", -1.5, 0.5, -0.5, 1.5, 0, green)
UsdGeom.PrimvarsAPI(ri).CreatePrimvar("ri:attributes:Ri:Matte", Sdf.ValueTypeNames.Int).Set(1)
karma = quad("/karma", 0.5, 0.5, 1.5, 1.5, 0, green)
UsdGeom.PrimvarsAPI(karma).CreatePrimvar("karma:object:holdoutmode", Sdf.ValueTypeNames.Int).Set(1)
quad("/inPass", -1.5, -1.5, -0.5, -0.5, 0, green)
quad("/plain", 0.5, -1.5, 1.5, -0.5, 0, green)
renderPass = UsdRender.Pass.Define(stage, "/passes/matte")
Usd.CollectionAPI.Apply(renderPass.GetPrim(), "matte").CreateIncludesRel().AddTarget("/inPass")

camera = UsdGeom.Camera.Define(stage, "/cam")
camera.CreateProjectionAttr("orthographic")
camera.CreateHorizontalApertureAttr(40.0)  # tenths of a unit: 4 units across
camera.CreateVerticalApertureAttr(40.0)
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 5))
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRendererSetting("hdvk:postProcess", False)
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
params.clearColor = Gf.Vec4f(0, 0, 0, 1)


def render():
    # Hydra presents the colour over the framebuffer, so zero-alpha
    # holdouts show what was there: clear it as an app does every frame.
    gl = glContext.functions()
    gl.glClearColor(0, 0, 0, 1)
    gl.glClear(0x4000)  # GL_COLOR_BUFFER_BIT
    for _ in range(2):
        engine.Render(stage.GetPseudoRoot(), params)
    return fbo.toImage()


def rgb(image, x, y):  # world (x, y) in [-2, 2]
    c = image.pixelColor(int((x + 2) / 4 * SIZE), int((2 - y) / 4 * SIZE))
    return (c.redF(), c.greenF(), c.blueF())


def black(c):
    return max(c) < 0.03


image = render()
expect(rgb(image, 0, 0)[0] > 0.9, "the wall is red")
expect(black(rgb(image, -1, 1)), "primvars:ri:attributes:Ri:Matte 1: a holdout")
expect(black(rgb(image, 1, 1)), "primvars:karma:object:holdoutmode 1: a holdout")
expect(rgb(image, 1, -1)[1] > 0.9 and rgb(image, -1, -1)[1] > 0.9, "the others as usual")
engine.SetActiveRenderPassPrimPath(Sdf.Path("/passes/matte"))
image = render()
expect(black(rgb(image, -1, -1)) and rgb(image, 1, -1)[1] > 0.9, "the active render pass's matte collection")
engine.SetActiveRenderPassPrimPath(Sdf.Path())
image = render()
expect(rgb(image, -1, -1)[1] > 0.9, "no active render pass: no matte collection")
engine.SetRendererSetting("hdvk:holdouts", False)
image = render()
expect(rgb(image, -1, 1)[1] > 0.9, "hdvk:holdouts off: drawn as usual")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
