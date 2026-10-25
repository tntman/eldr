"""Headless check of HdVk's lights and lighting effects through Hydra:
cylinder lights, soft sun shadows, volumetric fog, the reflection probe,
planar reflections and light textures.
Run: usdenv.cmd python tests\\test_lighting.py"""
import os
import sys
import tempfile

from PySide6.QtCore import QSize
from PySide6.QtGui import QColor, QImage, QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdShade

SIZE = 64

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def render(stage, settings={}):
    """The stage through /cam with HdVk, as a QImage; no TAA, fixed exposure."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for key, value in {"hdvk:taa": False, "hdvk:autoExposure": False, **settings}.items():
        engine.SetRendererSetting(key, value)
    frustum = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default()).frustum
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.clearColor = Gf.Vec4f(0, 0, 0, 1)
    for _ in range(8):
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    image = fbo.toImage()
    del engine  # release GL resources while the context is still current
    return image


def gray(image, x, y):
    """0..255 green at pixel (x, y), y down."""
    return image.pixelColor(x, y).green()


def wall_stage():
    """A white wall at z = 0 seen by /cam from z = 5."""
    stage = Usd.Stage.CreateInMemory()
    wall = UsdGeom.Mesh.Define(stage, "/wall")
    wall.CreatePointsAttr([(-4, -4, 0), (4, -4, 0), (4, 4, 0), (-4, 4, 0)])
    wall.CreateFaceVertexCountsAttr([4])
    wall.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    wall.CreateDisplayColorAttr([(1, 1, 1)])
    camera = UsdGeom.Camera.Define(stage, "/cam")
    camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 5))
    return stage


# Cylinder light: a horizontal tube in front of the wall lights a band
# across it; turned upright, the band turns with it.
stage = wall_stage()
tube = UsdLux.CylinderLight.Define(stage, "/tube")
tube.CreateLengthAttr(4)
tube.CreateRadiusAttr(0.05)
tube.CreateIntensityAttr(20)
tube.AddTranslateOp().Set(Gf.Vec3d(0, 0, 0.2))
rotate = tube.AddRotateZOp()
image = render(stage)
expect(gray(image, 32, 32) > 100, "cylinder light lights the wall")
expect(gray(image, 54, 32) > 2 * gray(image, 32, 54), "cylinder light along x: a horizontal band")
rotate.Set(90)
image = render(stage)
expect(gray(image, 32, 54) > 2 * gray(image, 54, 32), "rotated cylinder light: a vertical band")
image = render(stage, {"hdvk:localLights": False})
expect(gray(image, 32, 32) < 5, "hdvk:localLights off: no cylinder light")

# Soft sun shadows: a square floating one unit over the ground, seen from
# above, under a 10 degree sun: its shadow's edges spread over a penumbra
# of about 2 * tan(5 degrees) = 0.17 units (2 pixels) each side.
stage = wall_stage()
UsdGeom.Xformable(stage.GetPrimAtPath("/wall")).AddTranslateOp().Set(Gf.Vec3d(0, 0, -1))
square = UsdGeom.Mesh.Define(stage, "/square")
square.CreatePointsAttr([(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)])
square.CreateFaceVertexCountsAttr([4])
square.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
square.CreateDisplayColorAttr([(0, 0, 0)])
square.AddTranslateOp().Set(Gf.Vec3d(1, 0, 1))
sun = UsdLux.DistantLight.Define(stage, "/sun")
sun.CreateIntensityAttr(2)
sun.CreateNormalizeAttr(True)
sun.CreateAngleAttr(10)
sun.AddRotateYOp().Set(30)  # shadow shifted towards -x, its left edge in the open


def penumbra(image):
    """Partly lit pixels along the middle row, left of the square."""
    lit = gray(image, 2, 32)
    return sum(0.15 * lit < gray(image, x, 32) < 0.85 * lit for x in range(2, 30))


soft, hard = penumbra(render(stage)), penumbra(render(stage, {"hdvk:softSunShadows": False}))
expect(soft >= hard + 2, "soft sun shadows from the distant light's angle")
sun.CreateAngleAttr(0)
expect(penumbra(render(stage)) <= hard + 1, "a sun without angle casts hard shadows")

# Volumetric fog through the render settings: red fog (isotropic) 5 units
# deep in front of the wall a sun behind the camera lights: it hides most
# of the wall (exp(-1.5)) and scatters red light back.
stage = wall_stage()
sun = UsdLux.DistantLight.Define(stage, "/sun")
sun.CreateIntensityAttr(4)
sun.CreateNormalizeAttr(True)
linear = {"hdvk:postProcess": False}
clear = render(stage, linear).pixelColor(32, 32)
foggy = render(stage, {**linear, "hdvk:fog": True, "hdvk:fogDensity": 0.3, "hdvk:fogAnisotropy": 0.0,
                       "hdvk:fogColor": "1 0 0"}).pixelColor(32, 32)
expect(clear.green() == 255 and 40 < foggy.green() < 110, "fog hides the wall behind it")
expect(foggy.red() > foggy.green() + 30, "hdvk:fogColor: red fog scatters red light")

# Reflections of what the camera can't see: a chrome wall facing the camera
# and an emitter behind the camera over the left (x < 0). The reflection
# probe and planar reflections (the wall flagged with
# primvars:hdvk:planarReflection) show it on the left half of the wall;
# without them the wall stays dark.
def material(stage, path, **inputs):
    mat = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/pbr")
    shader.CreateIdAttr("UsdPreviewSurface")
    for name, value in inputs.items():
        kind = Sdf.ValueTypeNames.Color3f if isinstance(value, tuple) else Sdf.ValueTypeNames.Float
        shader.CreateInput(name, kind).Set(value)
    mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
    return mat


stage = wall_stage()
chrome = material(stage, "/chrome", diffuseColor=(1.0, 1.0, 1.0), metallic=1.0, roughness=0.05)
wall = stage.GetPrimAtPath("/wall")
UsdShade.MaterialBindingAPI.Apply(wall).Bind(chrome)
UsdGeom.PrimvarsAPI(wall).CreatePrimvar("hdvk:planarReflection", Sdf.ValueTypeNames.Bool,
                                        UsdGeom.Tokens.constant).Set(True)
emitter = UsdGeom.Mesh.Define(stage, "/emitter")
emitter.CreatePointsAttr([(-6, -6, 8), (0, -6, 8), (0, 6, 8), (-6, 6, 8)])
emitter.CreateFaceVertexCountsAttr([4])
emitter.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
glow = material(stage, "/glow", diffuseColor=(0.0, 0.0, 0.0), emissiveColor=(1.0, 1.0, 1.0))
UsdShade.MaterialBindingAPI.Apply(emitter.GetPrim()).Bind(glow)
for settings, what in (({"hdvk:reflectionProbe": True}, "hdvk:reflectionProbe"),
                       ({"hdvk:planarReflections": True}, "hdvk:planarReflections")):
    dark = render(stage, linear)
    lit = render(stage, {**linear, **settings})
    expect(gray(dark, 16, 32) < 10 and gray(lit, 16, 32) > 150 and gray(lit, 48, 32) < 10,
           what + ": the emitter behind the camera in the chrome wall")

# An authored reflection probe: a Cube flagged with
# primvars:hdvk:reflectionProbe around the wall's left part and the camera.
# It isn't drawn (from inside it the camera would see only its faces); the
# wall's left shows the emitter through it, the right only the black sky.
probe = UsdGeom.Cube.Define(stage, "/probe")
probe.AddTranslateOp().Set(Gf.Vec3d(-3.05, 0, 4))
probe.AddScaleOp().Set(Gf.Vec3f(5.9, 8, 10))
UsdGeom.PrimvarsAPI(probe).CreatePrimvar("hdvk:reflectionProbe", Sdf.ValueTypeNames.Bool,
                                         UsdGeom.Tokens.constant).Set(True)
lit = render(stage, {**linear, "hdvk:reflectionProbe": True})
expect(gray(lit, 8, 32) > 150 and gray(lit, 48, 32) < 10, "hdvk:authoredProbes: a Cube flagged as a probe")

# A textured rect light (inputs:texture:file; st (0,0) at +X +Y): black over its +X half,
# white over its -X half, close to the wall: the wall is dark under the
# black half, lit under the white one; hdvk:lightTextures off: lit evenly.
pattern = QImage(64, 2, QImage.Format_RGB32)
pattern.fill(QColor(255, 255, 255))
for x in range(32):
    for y in range(2):
        pattern.setPixelColor(x, y, QColor(0, 0, 0))
patternFile = os.path.join(tempfile.mkdtemp(), "halves.png")
pattern.save(patternFile)
stage = wall_stage()
panel = UsdLux.RectLight.Define(stage, "/panel")
panel.CreateWidthAttr(6)
panel.CreateHeightAttr(2)
panel.CreateIntensityAttr(4)
panel.CreateTextureFileAttr(patternFile)
panel.AddTranslateOp().Set(Gf.Vec3d(0, 0, 0.3))
image = render(stage, linear)
expect(gray(image, 52, 32) < 0.2 * gray(image, 12, 32) and gray(image, 12, 32) > 50,
       "rect light texture: the wall is dark under its black half")
image = render(stage, {**linear, "hdvk:lightTextures": False})
expect(abs(gray(image, 12, 32) - gray(image, 52, 32)) < 10, "hdvk:lightTextures off: lit evenly")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
