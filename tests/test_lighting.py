"""Headless check of HdVk's lights and lighting effects through Hydra:
cylinder lights, soft sun shadows, volumetric fog, the reflection probe,
planar reflections, light textures, light linking and IES profiles.
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
    for key, value in {"eldr:taa": False, "eldr:autoExposure": False, **settings}.items():
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
image = render(stage, {"eldr:localLights": False})
expect(gray(image, 32, 32) < 5, "eldr:localLights off: no cylinder light")

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


soft, hard = penumbra(render(stage)), penumbra(render(stage, {"eldr:softSunShadows": False}))
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
linear = {"eldr:postProcess": False}
clear = render(stage, linear).pixelColor(32, 32)
foggy = render(stage, {**linear, "eldr:fog": True, "eldr:fogDensity": 0.3, "eldr:fogAnisotropy": 0.0,
                       "eldr:fogColor": "1 0 0"}).pixelColor(32, 32)
expect(clear.green() == 255 and 40 < foggy.green() < 110, "fog hides the wall behind it")
expect(foggy.red() > foggy.green() + 30, "eldr:fogColor: red fog scatters red light")

# Reflections of what the camera can't see: a chrome wall facing the camera
# and an emitter behind the camera over the left (x < 0). The reflection
# probe and planar reflections (the wall flagged with
# primvars:eldr:planarReflection) show it on the left half of the wall;
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
UsdGeom.PrimvarsAPI(wall).CreatePrimvar("eldr:planarReflection", Sdf.ValueTypeNames.Bool,
                                        UsdGeom.Tokens.constant).Set(True)
emitter = UsdGeom.Mesh.Define(stage, "/emitter")
emitter.CreatePointsAttr([(-6, -6, 8), (0, -6, 8), (0, 6, 8), (-6, 6, 8)])
emitter.CreateFaceVertexCountsAttr([4])
emitter.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
glow = material(stage, "/glow", diffuseColor=(0.0, 0.0, 0.0), emissiveColor=(1.0, 1.0, 1.0))
UsdShade.MaterialBindingAPI.Apply(emitter.GetPrim()).Bind(glow)
for settings, what in (({"eldr:reflectionProbe": True}, "eldr:reflectionProbe"),
                       ({"eldr:planarReflections": True}, "eldr:planarReflections")):
    dark = render(stage, linear)
    lit = render(stage, {**linear, **settings})
    expect(gray(dark, 16, 32) < 10 and gray(lit, 16, 32) > 150 and gray(lit, 48, 32) < 10,
           what + ": the emitter behind the camera in the chrome wall")

# An authored reflection probe: a Cube flagged with
# primvars:eldr:reflectionProbe around the wall's left part and the camera.
# It isn't drawn (from inside it the camera would see only its faces); the
# wall's left shows the emitter through it, the right only the black sky.
probe = UsdGeom.Cube.Define(stage, "/probe")
probe.AddTranslateOp().Set(Gf.Vec3d(-3.05, 0, 4))
probe.AddScaleOp().Set(Gf.Vec3f(5.9, 8, 10))
UsdGeom.PrimvarsAPI(probe).CreatePrimvar("eldr:reflectionProbe", Sdf.ValueTypeNames.Bool,
                                         UsdGeom.Tokens.constant).Set(True)
lit = render(stage, {**linear, "eldr:reflectionProbe": True})
expect(gray(lit, 8, 32) > 150 and gray(lit, 48, 32) < 10, "eldr:authoredProbes: a Cube flagged as a probe")

# A textured rect light (inputs:texture:file; st (0,0) at +X +Y): black over its +X half,
# white over its -X half, close to the wall: the wall is dark under the
# black half, lit under the white one; eldr:lightTextures off: lit evenly.
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
image = render(stage, {**linear, "eldr:lightTextures": False})
expect(abs(gray(image, 12, 32) - gray(image, 52, 32)) < 10, "eldr:lightTextures off: lit evenly")

# As Karma, the image's top row is at the light's +Y (not UsdLux's st (0,0)
# corner): a white top half and a black bottom half light the wall's upper half.
vertical = QImage(2, 64, QImage.Format_RGB32)
vertical.fill(QColor(0, 0, 0))
for y in range(32):
    for x in range(2):
        vertical.setPixelColor(x, y, QColor(255, 255, 255))
verticalFile = os.path.join(tempfile.mkdtemp(), "top_white.png")
vertical.save(verticalFile)
stage = wall_stage()
panel = UsdLux.RectLight.Define(stage, "/panel")
panel.CreateWidthAttr(2)
panel.CreateHeightAttr(6)
panel.CreateIntensityAttr(4)
panel.CreateTextureFileAttr(verticalFile)
panel.AddTranslateOp().Set(Gf.Vec3d(0, 0, 0.3))
image = render(stage, linear)
expect(gray(image, 32, 52) < 0.2 * gray(image, 32, 12) and gray(image, 32, 12) > 50,
       "rect light texture: the image's top row is at +Y (Karma)")

# Ray-traced shadows don't shadow the surface itself far from the origin:
# a grey floor of 16x16 quads at z = 0.04 from (200, 200) to (400, 400), as Kitchen_set's
# tiles (centimetres), under a 35 degree sun, is as bright traced as
# unshadowed (their ray origins were under the floor half the time).
stage = Usd.Stage.CreateInMemory()
floor = UsdGeom.Mesh.Define(stage, "/floor")
floor.CreatePointsAttr([(200 + 200 * i / 16, 200 + 200 * j / 16, 0.04) for j in range(17) for i in range(17)])
floor.CreateFaceVertexCountsAttr([4] * 256)
floor.CreateFaceVertexIndicesAttr([v for j in range(16) for i in range(16)
                                   for v in (j * 17 + i, j * 17 + i + 1, (j + 1) * 17 + i + 1, (j + 1) * 17 + i)])
floor.CreateDisplayColorAttr([(0.5, 0.5, 0.5)])
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTransformOp().Set(Gf.Matrix4d().SetLookAt(Gf.Vec3d(300, 150, 120), Gf.Vec3d(300, 300, 0),
                                                    Gf.Vec3d(0, 0, 1)).GetInverse())
sun = UsdLux.DistantLight.Define(stage, "/sun")
sun.CreateIntensityAttr(4)
sun.CreateNormalizeAttr(True)
sun.AddRotateXYZOp().Set(Gf.Vec3f(55, 0, 30))
lit = render(stage, {**linear, "eldr:shadows": False})
traced = render(stage, {**linear, "eldr:rtShadows": True})
dark = sum(gray(traced, x, y) < 0.9 * gray(lit, x, y) for x in range(8, 56, 2) for y in range(8, 56, 2))
expect(gray(lit, 32, 32) > 50 and dark == 0, "ray-traced shadows: no self-shadowing (%d of 576 darker)" % dark)

# Light linking: a sphere light whose collection:lightLink excludes the
# wall leaves it dark (eldr:lightLinking off: lit); one whose
# collection:shadowLink excludes a square between it and the wall lights
# the wall behind the square, with shadow maps and traced.
stage = wall_stage()
lamp = UsdLux.SphereLight.Define(stage, "/lamp")
lamp.CreateIntensityAttr(1000)
lamp.CreateRadiusAttr(0.05)
lamp.AddTranslateOp().Set(Gf.Vec3d(0, 0, 1))
link = UsdLux.LightAPI(lamp).GetLightLinkCollectionAPI()
link.CreateIncludeRootAttr(True)
link.CreateExcludesRel().AddTarget("/wall")
expect(gray(render(stage, linear), 32, 32) < 3, "light linking: an excluded wall is unlit")
expect(gray(render(stage, {**linear, "eldr:lightLinking": False}), 32, 32) > 50,
       "eldr:lightLinking off: the wall is lit")
link.GetExcludesRel().ClearTargets(True)
lamp.GetPrim().GetAttribute("xformOp:translate").Set(Gf.Vec3d(2, 0, 1))
square = UsdGeom.Mesh.Define(stage, "/square")
square.CreatePointsAttr([(-0.25, -0.25, 0), (0.25, -0.25, 0), (0.25, 0.25, 0), (-0.25, 0.25, 0)])
square.CreateFaceVertexCountsAttr([4])
square.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
square.CreateDisplayColorAttr([(0, 0, 0)])
square.AddTranslateOp().Set(Gf.Vec3d(1, 0, 0.5))
for mode, settings in (("shadow maps", {}), ("traced", {"eldr:rtShadows": True})):
    shadowed = render(stage, {**linear, **settings})
    expect(gray(shadowed, 32, 32) < 0.3 * gray(shadowed, 32, 8),
           "light linking: the square shadows the wall (%s)" % mode)
    shadowLink = UsdLux.LightAPI(lamp).GetShadowLinkCollectionAPI()
    shadowLink.CreateIncludeRootAttr(True)
    shadowLink.CreateExcludesRel().SetTargets(["/square"])
    unshadowed = render(stage, {**linear, **settings})
    expect(gray(unshadowed, 32, 32) > 0.7 * gray(unshadowed, 32, 8),
           "shadow linking: a square outside the shadow link casts none (%s)" % mode)
    shadowLink.GetExcludesRel().ClearTargets(True)

# IES profile (tests/scenes/textures/spot_asym.ies, a 40 degree spot from
# the light's -Z, brighter towards phi 0, the light's -X as in Karma): the
# wall is lit under the light, dark 35 degrees off axis, brighter on -X;
# eldr:lightIes off: lit evenly.
stage = wall_stage()
spot = UsdLux.SphereLight.Define(stage, "/spot")
spot.CreateIntensityAttr(6)
spot.CreateRadiusAttr(0.01)
spot.AddTranslateOp().Set(Gf.Vec3d(0, 0, 1))
UsdLux.ShapingAPI.Apply(spot.GetPrim()).CreateShapingIesFileAttr(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "textures", "spot_asym.ies"))
image = render(stage, linear)
expect(gray(image, 32, 32) > 50 and gray(image, 56, 32) < 0.1 * gray(image, 32, 32),
       "IES profile: lit on its axis, dark past its beam (%d, %d)" % (gray(image, 32, 32), gray(image, 56, 32)))
expect(gray(image, 20, 32) > 1.5 * gray(image, 44, 32), "IES profile: brighter towards the light's -X (phi 0, Karma) (%d, %d)" % (gray(image, 20, 32), gray(image, 44, 32)))
spot.GetIntensityAttr().Set(6000)  # times the profile's peak, 1000 cd
image = render(stage, {**linear, "eldr:lightIes": False})
expect(gray(image, 56, 32) > 0.3 * gray(image, 32, 32), "eldr:lightIes off: no profile")

# IES pole: spot_asym.ies differs by phi at theta 0, so from the light's
# centre its pool pinches to a point. Karma evaluates it from each point
# on the light: a 0.1 radius light 1 above the wall blurs the pole over
# its extent. Seen orthographically, 0.6 wide (0.54 degrees a pixel from
# the light), the light over pixel (32, 32): the centre lies within a few %
# of its neighbours, and the -X : +X ratio at 2.67, 5.9 and 11.1 degrees
# is Karma's (husk 22.0.466, the same setup on a floor: 1.57, 2.52, 2.90).
stage = wall_stage()
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam"))
camera.CreateProjectionAttr(UsdGeom.Tokens.orthographic)
camera.CreateHorizontalApertureAttr(6)
camera.CreateVerticalApertureAttr(6)
spot = UsdLux.SphereLight.Define(stage, "/spot")
spot.CreateIntensityAttr(0.1)
spot.CreateRadiusAttr(0.1)
spot.AddTranslateOp().Set(Gf.Vec3d(0.6 / 128, -0.6 / 128, 1))  # pixel (32, 32)'s centre
UsdLux.ShapingAPI.Apply(spot.GetPrim()).CreateShapingIesFileAttr(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "textures", "spot_asym.ies"))
image = render(stage, linear)
c = gray(image, 32, 32)
near = [gray(image, 32 + dx, 32 + dy) for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1))]
expect(50 < c < 250 and all(abs(n / c - 1) < 0.08 for n in near),
       "IES pole: the pool's centre lies within 8%% of its neighbours (%d; %s)" % (c, near))
for k, karma in ((5, 1.57), (11, 2.52), (21, 2.90)):
    ratio = gray(image, 32 - k, 32) / max(gray(image, 32 + k, 32), 1)
    expect(abs(ratio / karma - 1) < 0.1, "IES pole: -X : +X %d px off the pole %.2f, Karma %.2f" % (k, ratio, karma))

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
