"""Headless check of HdVk's OpenVDB volumes (built against a USD with
OpenVDB): tests/scenes/volume.vdb's cloud in front of an emissive wall, no
lights, so it only absorbs. It darkens the wall where it is, the volume's
transform moves it, fieldIndex 1 picks the file's second "density" grid (a
small ball), a MaterialX volume material sets its absorption and emission,
half resolution stays close, eldr:volumes switches volumes off, and the
render stats count them and their grid memory.
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
engine.SetRendererSetting("eldr:autoExposure", False)
params = UsdImagingGL.RenderParams()


def render():
    for _ in range(40):  # until TAA converges
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    return fbo.toImage()


def color(image, x, y):
    """The pixel showing world (x, y, 0)."""
    p = viewProj.Transform(Gf.Vec3d(x, y, 0))
    px, py = int((p[0] * 0.5 + 0.5) * SIZE), int((0.5 - p[1] * 0.5) * SIZE)  # QImage rows top down
    return image.pixelColor(px, py)


def brightness(image, x, y):
    """Green of the pixel showing world (x, y, 0)."""
    return color(image, x, y).greenF()


def shader(path, id, **inputs):
    """A MaterialX node with constant inputs and an `out` output."""
    node = UsdShade.Shader.Define(stage, path)
    node.CreateIdAttr(id)
    for name, value in inputs.items():
        node.CreateInput(name, Sdf.ValueTypeNames.Vector3f).Set(value)
    node.CreateOutput("out", Sdf.ValueTypeNames.Token)
    return node


failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


image = render()
wallLevel = brightness(image, 3.5, 3.5)
expect(wallLevel > 0.2, "the wall glows")
cloudLevel = brightness(image, 0, 0)
expect(cloudLevel < 0.5 * wallLevel, "the cloud absorbs the wall's light")
expect(brightness(image, 1.0, 0) < 0.5 * wallLevel, "the cloud is 1.6 across")
stats = engine.GetRenderStats()
expect(stats.get("volumes") == 1 and stats.get("volumeMemory", 0) > 0, "stats count volumes and grid memory")
expect("volumes" in stats.get("gpuPassMs", {}), "the volumes' GPU pass is timed")
rows = dict(hdvkUsdview.Rows(stats))
expect("Volumes" in rows and "  volumes" in rows, "the usdview window shows volumes")

translate.Set(Gf.Vec3d(2.5, 0, 0))
image = render()
expect(brightness(image, 0, 0) > 0.9 * wallLevel and brightness(image, 2.5, 0) < 0.5 * wallLevel,
       "the volume's transform moves it")
translate.Set(Gf.Vec3d(0, 0, 0))

field.CreateFieldIndexAttr(1)  # the second "density": a ball of radius 0.5
image = render()
expect(brightness(image, 0, 0) < 0.5 * wallLevel and brightness(image, 1.0, 0) > 0.9 * wallLevel,
       "fieldIndex picks the grid")

field.GetFieldIndexAttr().Set(0)

# A MaterialX volume material (the mtlx:volume terminal): absorption 0.1 per
# unit of density (without a material: extinction 1) lets the wall through;
# a teal uniform_edf glows per unit of density.
material = UsdShade.Material.Define(stage, "/smoke")
volumeShader = shader("/smoke/volume", "ND_volume")
vdf = shader("/smoke/vdf", "ND_absorption_vdf", absorption=(0.1, 0.1, 0.1))
volumeShader.CreateInput("vdf", Sdf.ValueTypeNames.Token).ConnectToSource(vdf.ConnectableAPI(), "out")
material.CreateOutput("mtlx:volume", Sdf.ValueTypeNames.Token).ConnectToSource(volumeShader.ConnectableAPI(), "out")
UsdShade.MaterialBindingAPI.Apply(volume.GetPrim()).Bind(material)
image = render()
expect(brightness(image, 0, 0) > cloudLevel + 0.1, "a volume material's absorption")
edf = UsdShade.Shader.Define(stage, "/smoke/edf")
edf.CreateIdAttr("ND_uniform_edf")
edf.CreateInput("color", Sdf.ValueTypeNames.Color3f).Set((0, 0.4, 0.4))
edf.CreateOutput("out", Sdf.ValueTypeNames.Token)
volumeShader.CreateInput("edf", Sdf.ValueTypeNames.Token).ConnectToSource(edf.ConnectableAPI(), "out")
image = render()
glow = color(image, 0, 0)
expect(glow.greenF() > glow.redF() + 0.15, "a volume material's emission")
UsdShade.MaterialBindingAPI(volume.GetPrim()).UnbindAllBindings()

# Half resolution: the same image, near enough, the cloud's edge included.
full = render()
engine.SetRendererSetting("eldr:volumeResolution", 2)
image = render()
expect(all(abs(brightness(image, x, 0) - brightness(full, x, 0)) < 0.05 for x in (0, 1.0, 1.6, 2.5)),
       "half resolution volumes")
engine.SetRendererSetting("eldr:volumeResolution", 1)

engine.SetRendererSetting("eldr:volumes", False)
image = render()
expect(brightness(image, 0, 0) > 0.9 * wallLevel, "eldr:volumes switches volumes off")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
