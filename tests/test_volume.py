"""Headless check of HdVk's OpenVDB volumes (built against a USD with
OpenVDB): tests/scenes/volume.vdb's cloud in front of an emissive wall, no
lights, so it only absorbs. It darkens the wall where it is, the volume's
transform moves it, fieldIndex 1 picks the file's second "density" grid (a
small ball), a MaterialX volume material sets its absorption and emission,
remaps and ramps of the density (MaterialX's and Houdini's) shape them,
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
absorbed = color(image, 0, 0)
expect(absorbed.greenF() > cloudLevel + 0.1, "a volume material's absorption")
edf = UsdShade.Shader.Define(stage, "/smoke/edf")
edf.CreateIdAttr("ND_uniform_edf")
edf.CreateInput("color", Sdf.ValueTypeNames.Color3f).Set((0, 0.4, 0.4))
edf.CreateOutput("out", Sdf.ValueTypeNames.Token)
volumeShader.CreateInput("edf", Sdf.ValueTypeNames.Token).ConnectToSource(edf.ConnectableAPI(), "out")
image = render()
glow = color(image, 0, 0)
expect(glow.greenF() > glow.redF() + 0.15, "a volume material's emission")
UsdShade.MaterialBindingAPI(volume.GetPrim()).UnbindAllBindings()


def node(path, id, **inputs):
    """A MaterialX node: inputs (type, value or upstream node) and an `out` output."""
    n = UsdShade.Shader.Define(stage, path)
    n.CreateIdAttr(id)
    for name, (type, value) in inputs.items():
        if isinstance(value, UsdShade.Shader):
            n.CreateInput(name, type).ConnectToSource(value.ConnectableAPI(), "out")
        else:
            n.CreateInput(name, type).Set(value)
    n.CreateOutput("out", Sdf.ValueTypeNames.Token)
    return n


# Coloured absorption as Houdini's Material Library writes it: a constant
# colour node times the density field (geompropvalue). Green and blue are
# absorbed, red shows through: the cloud is red, over the grey wall. Once
# the constant node wasn't read (grey 1) and transmittance was grey.
C3, F, V3, S = (Sdf.ValueTypeNames.Color3f, Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Vector3f,
                Sdf.ValueTypeNames.String)
tinted = UsdShade.Material.Define(stage, "/tinted")
density = node("/tinted/density", "ND_geompropvalue_float", geomprop=(S, "density"))
constant = node("/tinted/constant", "ND_constant_color3", value=(C3, (0.05, 3, 3)))
product = node("/tinted/product", "ND_multiply_color3FA", in1=(C3, constant), in2=(F, density))
tintedVdf = node("/tinted/vdf", "ND_anisotropic_vdf", absorption=(V3, product))
tintedVolume = node("/tinted/volume", "ND_volume", vdf=(Sdf.ValueTypeNames.Token, tintedVdf))
tinted.CreateOutput("mtlx:volume", Sdf.ValueTypeNames.Token).ConnectToSource(tintedVolume.ConnectableAPI(), "out")
UsdShade.MaterialBindingAPI.Apply(volume.GetPrim()).Bind(tinted)
image = render()
red = color(image, 0, 0)
expect(red.redF() > 0.5 * wallLevel and red.greenF() < 0.2 * red.redF(),
       "coloured absorption from a constant colour node (%.2f %.2f %.2f, wall %.2f, cloud %.2f)"
       % (red.redF(), red.greenF(), red.blueF(), wallLevel, cloudLevel))

# A node volume graphs don't read (noise): the whole material is ignored,
# the cloud renders as without one.
product.GetInput("in1").ConnectToSource(node("/tinted/noise", "ND_noise3d_color3").ConnectableAPI(), "out")
image = render()
expect(abs(brightness(image, 0, 0) - cloudLevel) < 0.05, "an unread volume graph renders as without a material")

# Karma's kma_volume (the kma:volume terminal) with the same coloured absorption.
product.GetInput("in1").ConnectToSource(constant.ConnectableAPI(), "out")
karma = UsdShade.Material.Define(stage, "/karma")
kmaVolume = node("/karma/volume", "kma_volume", absorption=(C3, product))
karma.CreateOutput("kma:volume", Sdf.ValueTypeNames.Token).ConnectToSource(kmaVolume.ConnectableAPI(), "out")
UsdShade.MaterialBindingAPI(volume.GetPrim()).Bind(karma)
image = render()
red = color(image, 0, 0)
expect(red.redF() > 0.5 * wallLevel and red.greenF() < 0.2 * red.redF(), "Karma's kma_volume")
UsdShade.MaterialBindingAPI(volume.GetPrim()).UnbindAllBindings()

# Functions of a field (baked into tables over its range, volume.cpp's
# FieldGraph): a MaterialX remap of the density onto 0..0.1 absorbs as 0.1
# per unit of density; so does Houdini's ramp as its MtlX Float Ramp VOP
# writes it (hinvlinear + huniformramp: keys 0, 10, values 0, 1).
F2, I, C4, T = (Sdf.ValueTypeNames.Float2, Sdf.ValueTypeNames.Int, Sdf.ValueTypeNames.Color4f,
                Sdf.ValueTypeNames.Token)
graphs = UsdShade.Material.Define(stage, "/graphs")
densityRead = node("/graphs/density", "ND_geompropvalue_float", geomprop=(S, "density"))
remap = node("/graphs/remap", "ND_remap_float", **{"in": (F, densityRead)}, outhigh=(F, 0.1))
graphVdf = node("/graphs/vdf", "ND_absorption_vdf", absorption=(V3, remap))
graphVolume = node("/graphs/volume", "ND_volume", vdf=(T, graphVdf))
graphs.CreateOutput("mtlx:volume", T).ConnectToSource(graphVolume.ConnectableAPI(), "out")
UsdShade.MaterialBindingAPI(volume.GetPrim()).Bind(graphs)
image = render()
expect(abs(brightness(image, 0, 0) - absorbed.greenF()) < 0.03,
       "a remap of the density (%.3f, absorption 0.1: %.3f)" % (brightness(image, 0, 0), absorbed.greenF()))
count = node("/graphs/count", "ND_constant_float", value=(F, 2.0))
keys = node("/graphs/keys", "ND_hinvlinear_float", ramp=(F, densityRead), num=(F, count),
            **{"key%d" % i: (F, 10.0) for i in range(1, 10)})
values = node("/graphs/values", "ND_huniformramp_float", ramp=(F, keys), num=(F, count), value1=(F, 1.0))
graphVdf.GetInput("absorption").ConnectToSource(values.ConnectableAPI(), "out")
image = render()
expect(abs(brightness(image, 0, 0) - absorbed.greenF()) < 0.03,
       "Houdini's ramp of the density (%.3f, absorption 0.1: %.3f)" % (brightness(image, 0, 0), absorbed.greenF()))
# A fire: MaterialX's ramp (two keys, black to orange by density 0.2) of
# the density glows orange per unit length, not per unit of density.
graphVdf.GetInput("absorption").ConnectToSource(remap.ConnectableAPI(), "out")
texcoord = node("/graphs/texcoord", "ND_combine2_vector2", in1=(F, densityRead))
ramp = node("/graphs/ramp", "ND_ramp", texcoord=(F2, texcoord), interpolation=(I, 0), interval2=(F, 0.2),
            color1=(C4, (0, 0, 0, 1)), color2=(C4, (1, 0.3, 0, 1)))
rgb = node("/graphs/rgb", "ND_convert_color4_color3", **{"in": (C4, ramp)})
graphEdf = node("/graphs/edf", "ND_uniform_edf", color=(C3, rgb))
graphVolume.CreateInput("edf", T).ConnectToSource(graphEdf.ConnectableAPI(), "out")
image = render()
fire = color(image, 0, 0)
red, green = fire.redF() - absorbed.redF(), fire.greenF() - absorbed.greenF()
blue = fire.blueF() - absorbed.blueF()
expect(red > 0.15 and green < 0.7 * red and blue < 0.7 * green,  # orange, through the tonemapper
       "a ramp of the density to emission (%.2f %.2f %.2f over %.2f %.2f %.2f)"
       % (fire.redF(), fire.greenF(), fire.blueF(), absorbed.redF(), absorbed.greenF(), absorbed.blueF()))
UsdShade.MaterialBindingAPI(volume.GetPrim()).UnbindAllBindings()

# Half resolution: the same image, near enough, the cloud's edge included.
full = render()
engine.SetRendererSetting("eldr:volumeResolution", 2)
image = render()
expect(all(abs(brightness(image, x, 0) - brightness(full, x, 0)) < 0.05 for x in (0, 1.0, 1.6, 2.5)),
       "half resolution volumes")
engine.SetRendererSetting("eldr:volumeResolution", 1)

# Volumes under glass (VolumePass::TranslucentLayers): the glowing cloud
# through two grey thin panes (each letting about half through) is dimmed by
# both; with the far pane moved behind the cloud, by the near one only. Once
# only the nearest layer split the ray, and the cloud between the panes was
# dimmed by both too. Linear (no post) for the ratios.
UsdShade.MaterialBindingAPI(volume.GetPrim()).Bind(material)
edf.GetInput("color").Set((0, 0.2, 0.2))
glass = UsdShade.Material.Define(stage, "/glass")
glassShader = UsdShade.Shader.Define(stage, "/glass/surface")
glassShader.CreateIdAttr("ND_standard_surface_surfaceshader")
glassShader.CreateInput("transmission", Sdf.ValueTypeNames.Float).Set(1.0)
glassShader.CreateInput("transmission_color", Sdf.ValueTypeNames.Color3f).Set((0.5, 0.5, 0.5))
glassShader.CreateInput("thin_walled", Sdf.ValueTypeNames.Bool).Set(True)
glass.CreateSurfaceOutput("mtlx").ConnectToSource(glassShader.CreateOutput("out", Sdf.ValueTypeNames.Token))
panes = {}
for name, z in (("near", 3.0), ("far", 2.0)):
    pane = UsdGeom.Mesh.Define(stage, "/" + name)
    pane.CreatePointsAttr([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)])
    pane.CreateFaceVertexCountsAttr([4])
    pane.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    UsdShade.MaterialBindingAPI.Apply(pane.GetPrim()).Bind(glass)
    panes[name] = pane.AddTranslateOp()
    panes[name].Set(Gf.Vec3d(0, 0, z))
engine.SetRendererSetting("eldr:postProcess", False)
behindBoth = color(render(), 0, 0).greenF()
panes["far"].Set(Gf.Vec3d(0, 0, -3))
between = color(render(), 0, 0).greenF()
for name in panes:
    stage.GetPrimAtPath("/" + name).SetActive(False)
bare = color(render(), 0, 0).greenF()
print("cloud green: bare %.3f, behind two panes %.3f, between them %.3f" % (bare, behindBoth, between))
expect(0.15 * bare < behindBoth < 0.35 * bare, "a cloud behind two panes is dimmed by both")
expect(between > 1.3 * behindBoth, "a cloud between two panes is dimmed by the near one only")
engine.SetRendererSetting("eldr:postProcess", True)
UsdShade.MaterialBindingAPI(volume.GetPrim()).UnbindAllBindings()

engine.SetRendererSetting("eldr:volumes", False)
image = render()
expect(brightness(image, 0, 0) > 0.9 * wallLevel, "eldr:volumes switches volumes off")

# Ray-traced sun shadows: volumes look no darker for it (volume.frag's
# cascade lookup once fell into an empty cascade 0: mean 24.3 against 31.0 on
# volume.usda; the sun now keeps its cascades for volumes, test_traced_media.py).
del engine  # release GL resources while the context is still current
scene = Usd.Stage.Open(os.path.join(os.path.dirname(VDB), "volume.usda"))
sceneCam = UsdGeom.Camera(scene.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default()).frustum


def mean(**settings):
    engine = UsdImagingGL.Engine()
    engine.SetRendererPlugin("HdVkRendererPlugin")
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(sceneCam.ComputeViewMatrix(), sceneCam.ComputeProjectionMatrix())
    for key, value in {"eldr:autoExposure": False, **settings}.items():
        engine.SetRendererSetting(key, value)
    for _ in range(60):
        engine.Render(scene.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    image = fbo.toImage()
    del engine
    return sum(image.pixelColor(x, y).green() for x in range(SIZE) for y in range(SIZE)) / SIZE ** 2


maps, traced = mean(), mean(**{"eldr:rtShadows": True})
expect(traced > 0.95 * maps, "ray-traced sun: volumes as bright as with maps (%.1f, %.1f)" % (maps, traced))

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
