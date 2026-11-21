"""Renders the README banner (docs/images/banner.webp, banner.jpg) with Eldr through husk.
Run from the repo root: python docs\\site\\banner.py [layer] [render] [comp] [webp] (default: all).
layer: writes docs/site/banner_sponza.usda: Intel's Sponza (CC BY 4.0, not in the repo: SPONZA,
default E:/usd-testscenes/sponza) with its curtains, ivy and cypress add-ons, Eldr's feature objects
in the atrium, an animated camera and a final settings prim. render: husk with Eldr's Houdini
build (ELDR_H22_BUILD, default build-h22) into ELDR_BANNER_TMP (default: a folder in the system
temp); ELDR_BANNER_TEST=1 renders /Render/bannerTest (few samples), ELDR_BANNER_FRAMES="1 4 18"
first, count and step. comp: tonemaps each linear EXR (ACES 2.0 SDR) and lays the title over it.
webp: banner.jpg (frame 1) and the animated banner.webp with Pillow (24 fps, 960 wide)."""
import math
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HFS = pathlib.Path(os.environ.get("ELDR_HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466"))
H22 = pathlib.Path(os.environ.get("ELDR_H22_BUILD", ROOT / "build-h22"))
TMP = pathlib.Path(os.environ.get("ELDR_BANNER_TMP", pathlib.Path(tempfile.gettempdir()) / "eldr_banner"))
SPONZA = pathlib.Path(os.environ.get("SPONZA", "E:/usd-testscenes/sponza"))
SPONZA_LAYERS = [  # strongest first
    "pkg_a_curtains/NewSponza_Curtains_USD_YUp.usda",
    "pkg_b_ivy/NewSponza_IvyGrowth_USD_YUp.usda",
    "pkg_c_trees/NewSponza_CypressTree_USDA_YUp.usda",
    "main_sponza/NewSponza_Main_USD_Yup_003.usda",
]
LAYER = ROOT / "docs" / "site" / "banner_sponza.usda"
OIIO = HFS / "bin" / "hoiiotool.exe"
W, H, FRAMES, FPS = 1280, 480, 72, 24
WEBP_W = 960
EXPOSURE = 1.3  # linear gain before the display transform
SAMPLES, TEST_SAMPLES = 128, 16

# Scene units are centimetres (Sponza's). The camera dollies forward and sideways and pulls focus
# from a near object to the far marble sphere and back, all on sin/cos of the frame: frame 73 is frame 1.
EYE, LOOK = (740.0, 100.0, 35.0), (-400.0, 130.0, -10.0)
DOLLY, SWAY = 40.0, 30.0
FOCAL, H_APERTURE, F_STOP = 28.0, 36.0, 1.0
JADE, MARBLE = (230.0, 26.0, 115.0), (-60.0, 32.0, 150.0)  # the subsurface and the procedural sphere
FOCUS_NEAR, FOCUS_FAR = (250.0, 25.0, -60.0), MARBLE
SUN = (-0.3, -0.95, 0.1)  # the direction sunlight travels
GLASS, SPOT = (60.0, 32.0, 40.0), (60.0, 150.0, -170.0)  # the glass ball's centre, the IES spot
ORB_CENTRE, ORB_RADIUS, ORB_TURNS = (60.0, 95.0, 40.0), 80.0, 2


def run(cmd, env=None):
    print(" ".join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True, env=env)


def norm(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def matrix(p, forward):
    """A transform at p whose -Z looks along forward, +Y up."""
    z = norm([-c for c in forward])
    x = norm(cross((0, 1, 0), z))
    y = cross(z, x)
    rows = [x + [0], y + [0], z + [0], list(p) + [1]]
    return "( %s )" % ", ".join("(%s)" % ", ".join("%.6g" % v for v in r) for r in rows)


def phase(frame):
    return 2 * math.pi * (frame - 1) / FRAMES


def eye(frame):
    fwd = norm([LOOK[i] - EYE[i] for i in range(3)])
    side = norm(cross(fwd, (0, 1, 0)))
    t = phase(frame)
    d, s = DOLLY * (1 - math.cos(t)) / 2, SWAY * math.sin(t)
    return [EYE[i] + fwd[i] * d + side[i] * s for i in range(3)], fwd


def focus(frame):
    p, fwd = eye(frame)
    k = (1 - math.cos(phase(frame))) / 2  # 0 at frame 1: the near object is sharp
    target = [FOCUS_NEAR[i] + k * (FOCUS_FAR[i] - FOCUS_NEAR[i]) for i in range(3)]
    return sum((target[i] - p[i]) * fwd[i] for i in range(3))


def orb(t):
    a = 2 * math.pi * ORB_TURNS * (t - 1) / FRAMES
    return (ORB_CENTRE[0] + ORB_RADIUS * math.cos(a), ORB_CENTRE[1] + 8 * math.sin(2 * a),
            ORB_CENTRE[2] + ORB_RADIUS * math.sin(a))


def sphere(name, mtl, p, r):
    return '''def Sphere "%s" (prepend apiSchemas = ["MaterialBindingAPI"])
{
    double radius = %g
    rel material:binding = </banner/mtl/%s>
    double3 xformOp:translate = (%g, %g, %g)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
''' % (name, r, mtl, p[0], p[1], p[2])


def mtlx(name, node="ND_standard_surface_surfaceshader", **inputs):
    lines = "\n".join("            %s inputs:%s = %s" % ({str: "color3f", bool: "bool"}.get(type(v), "float"),
                                                         k, v if not isinstance(v, bool) else int(v))
                      for k, v in inputs.items())
    return '''    def Material "%(n)s"
    {
        token outputs:mtlx:surface.connect = </banner/mtl/%(n)s/surface.outputs:out>
        def Shader "surface"
        {
            uniform token info:id = "%(node)s"
%(l)s
            token outputs:out
        }
    }
''' % dict(n=name, l=lines, node=node)


# Marble from MaterialX nodes: veins where sin(x + turbulence) crosses 0, on a polished white stone.
MARBLE_MTL = '''    def Material "marble"
    {
        token outputs:mtlx:surface.connect = </banner/mtl/marble/surface.outputs:out>
        def Shader "surface"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color.connect = </banner/mtl/marble/colour.outputs:out>
            float inputs:coat = 0.6
            float inputs:coat_roughness = 0.05
            float inputs:specular_roughness = 0.3
            token outputs:out
        }
        def Shader "position"
        {
            uniform token info:id = "ND_position_vector3"
            string inputs:space = "object"
            float3 outputs:out
        }
        def Shader "scaled"
        {
            uniform token info:id = "ND_multiply_vector3FA"
            float3 inputs:in1.connect = </banner/mtl/marble/position.outputs:out>
            float inputs:in2 = 0.05
            float3 outputs:out
        }
        def Shader "turbulence"
        {
            uniform token info:id = "ND_fractal3d_float"
            float inputs:amplitude = 1
            int inputs:octaves = 5
            float3 inputs:position.connect = </banner/mtl/marble/scaled.outputs:out>
            float outputs:out
        }
        def Shader "along"
        {
            uniform token info:id = "ND_dotproduct_vector3"
            float3 inputs:in1.connect = </banner/mtl/marble/scaled.outputs:out>
            float3 inputs:in2 = (2.5, 1.5, 0.8)
            float outputs:out
        }
        def Shader "warped"
        {
            uniform token info:id = "ND_add_float"
            float inputs:in1.connect = </banner/mtl/marble/along.outputs:out>
            float inputs:in2.connect = </banner/mtl/marble/turbulenceScaled.outputs:out>
            float outputs:out
        }
        def Shader "turbulenceScaled"
        {
            uniform token info:id = "ND_multiply_float"
            float inputs:in1.connect = </banner/mtl/marble/turbulence.outputs:out>
            float inputs:in2 = 4
            float outputs:out
        }
        def Shader "wave"
        {
            uniform token info:id = "ND_sin_float"
            float inputs:in.connect = </banner/mtl/marble/warped.outputs:out>
            float outputs:out
        }
        def Shader "distance"
        {
            uniform token info:id = "ND_absval_float"
            float inputs:in.connect = </banner/mtl/marble/wave.outputs:out>
            float outputs:out
        }
        def Shader "nearVein"
        {
            uniform token info:id = "ND_subtract_float"
            float inputs:in1 = 1
            float inputs:in2.connect = </banner/mtl/marble/distance.outputs:out>
            float outputs:out
        }
        def Shader "vein"
        {
            uniform token info:id = "ND_power_float"
            float inputs:in1.connect = </banner/mtl/marble/nearVein.outputs:out>
            float inputs:in2 = 3
            float outputs:out
        }
        def Shader "colour"
        {
            uniform token info:id = "ND_mix_color3"
            color3f inputs:bg = (0.9, 0.88, 0.84)
            color3f inputs:fg = (0.06, 0.07, 0.1)
            float inputs:mix.connect = </banner/mtl/marble/vein.outputs:out>
            color3f outputs:out
        }
    }
'''


def rel(path):
    return os.path.relpath(ROOT / path, LAYER.parent).replace("\\", "/")


def settings(name, samples):
    return '''    def RenderSettings "%s" (
        references = @%s@</Render/final>
    )
    {
        rel camera = </banner/cam>
        uniform int2 resolution = (%d, %d)
        custom bool eldr:denoise = 1
        custom int eldr:filmSamples = %d
        custom bool eldr:fog = 1
        custom float eldr:fogDensity = 0.00012
        custom float eldr:fogHeightFalloff = 0.001
        custom bool eldr:gi = 1
        custom bool eldr:motionBlur = 1
        custom bool eldr:rtShadows = 1
        custom int eldr:textureCompression = 1
    }
''' % (name, rel("tests/scenes/final_settings.usda"), W, H, samples)


def layer():
    missing = [l for l in SPONZA_LAYERS if not (SPONZA / l).exists()]
    if missing:
        sys.exit("banner.py: Intel Sponza is missing under %s (%s). Download the Sponza 2022 main scene and "
                 "the curtains, ivy and trees add-ons (CC BY 4.0) and set SPONZA to the folder." %
                 (SPONZA, ", ".join(missing)))
    cam = ",\n".join("            %d: %s" % (f, matrix(*eye(f))) for f in range(0, FRAMES + 2))
    foc = ",\n".join("            %d: %.2f" % (f, focus(f)) for f in range(0, FRAMES + 2))
    orbs = ",\n".join("            %g: (%.2f, %.2f, %.2f)" % ((t / 2,) + orb(t / 2)) for t in range(0, 2 * FRAMES + 4))
    # binding_api.usda applies MaterialBindingAPI to the add-ons' meshes (their files leave it out).
    sub = ",\n".join(["        @%s@" % rel("tests/scenes/production/binding_api.usda")] +
                     ["        @%s@" % (SPONZA / l).as_posix() for l in SPONZA_LAYERS])
    LAYER.write_text('''#usda 1.0
(
    doc = """README banner: written by docs/site/banner.py layer. Intel Sponza (CC BY 4.0) with
its curtains, ivy and cypress add-ons, read from %(sponza)s (not in the repo)."""
    endTimeCode = %(end)d
    framesPerSecond = %(fps)d
    metersPerUnit = 0.01
    renderSettingsPrimPath = "/Render/banner"
    startTimeCode = 1
    subLayers = [
%(sub)s
    ]
    timeCodesPerSecond = %(fps)d
    upAxis = "Y"
)

# The cypress add-on is in metres: scaled to Sponza's centimetres, moved to the back of the atrium.
over "Italian_Cypress_Dense_Custom_05b"
{
    matrix4d xformOp:transform = ( (100, 0, 0, 0), (0, 0, -100, 0), (0, 100, 0, 0), (-620, 0, 190, 1) )
}

def Xform "banner"
{
def Camera "cam"
{
    float focalLength = %(focal)g
    float fStop = %(fstop)g
    float focusDistance.timeSamples = {
%(foc)s
    }
    float horizontalAperture = %(hap)g
    float verticalAperture = %(vap).4f
    double shutter:open = -0.25
    double shutter:close = 0.25
    matrix4d xformOp:transform.timeSamples = {
%(cam)s
    }
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def DistantLight "sun"
{
    float inputs:angle = 0.53
    color3f inputs:color = (1, 0.9, 0.78)
    float inputs:intensity = 5
    bool inputs:normalize = 1
    matrix4d xformOp:transform = %(sun)s
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def DomeLight "sky"
{
    float inputs:intensity = 1
    asset inputs:texture:file = @%(sky)s@
}

# An IES spot (tests/scenes/cornell/beam.ies) low on one side of the glass ball: the ball focuses
# it into a caustic on the floor beyond.
def SphereLight "iesSpot" (prepend apiSchemas = ["ShapingAPI"])
{
    color3f inputs:color = (1, 0.8, 0.55)
    float inputs:exposure = 9
    float inputs:intensity = 1
    float inputs:radius = 5
    asset inputs:shaping:ies:file = @%(ies)s@
    matrix4d xformOp:transform = %(spot)s
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

%(spheres)s
def Sphere "glass" (prepend apiSchemas = ["MaterialBindingAPI"])
{
    bool primvars:karma:object:causticsenable = 1 (interpolation = "constant")
    double radius = 32
    rel material:binding = </banner/mtl/glass>
    double3 xformOp:translate = (%(glass_at)s)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

# A glowing orb circling the glass ball: it lights the floor and blurs with its motion.
def Sphere "orb" (prepend apiSchemas = ["MaterialBindingAPI"])
{
    double radius = 8
    rel material:binding = </banner/mtl/glow>
    double3 xformOp:translate.timeSamples = {
%(orbs)s
    }
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

def Scope "mtl"
{
%(glass)s%(chrome)s%(steel)s%(paint)s%(film)s%(jade)s%(marble)s
    def Material "glow"
    {
        token outputs:surface.connect = </banner/mtl/glow/surface.outputs:surface>
        def Shader "surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0, 0, 0)
            color3f inputs:emissiveColor = (40, 20, 8)
            token outputs:surface
        }
    }
}
}

def Scope "Render"
{
%(settings)s}
''' % dict(sponza=SPONZA.as_posix(), end=FRAMES, fps=FPS, sub=sub, focal=FOCAL, fstop=F_STOP, foc=foc,
           hap=H_APERTURE, vap=H_APERTURE * H / W, cam=cam, sun=matrix((0, 0, 0), SUN), glass_at="%g, %g, %g" % GLASS, spot=matrix(SPOT, [GLASS[i] - SPOT[i] for i in range(3)]), sky=(SPONZA / "main_sponza/textures/kloppenheim_05_4k.hdr").as_posix(), orbs=orbs,
           ies=rel("tests/scenes/cornell/beam.ies"),
           spheres="".join([sphere("chrome", "chrome", (110, 30, -120), 30),
                            sphere("brushed", "steel", (-200, 26, 215), 26),
                            sphere("carPaint", "paint", (250, 25, -60), 25),
                            sphere("thinFilm", "film", (-40, 26, -40), 26),
                            sphere("jade", "jade", JADE, 26),
                            sphere("marble", "marble", MARBLE, 30)]),
           glass=mtlx("glass", specular_IOR=1.5, specular_roughness=0.0, transmission=1.0),
           chrome=mtlx("chrome", base_color="(0.95, 0.95, 0.95)", metalness=1.0, specular_roughness=0.03),
           steel=mtlx("steel", base_color="(0.9, 0.9, 0.92)", metalness=1.0, specular_anisotropy=0.8,
                      specular_roughness=0.35),
           paint=mtlx("paint", base_color="(0.55, 0.03, 0.02)", coat=1.0, coat_roughness=0.03, metalness=0.5,
                      specular_roughness=0.45),
           film=mtlx("film", base_color="(0.6, 0.6, 0.62)", metalness=1.0, specular_roughness=0.2, thin_film_IOR=2.0,
                     thin_film_thickness=500.0),
           jade=mtlx("jade", "ND_open_pbr_surface_surfaceshader", base_color="(0.3, 0.6, 0.4)",
                     specular_roughness=0.15, subsurface_weight=1.0, subsurface_color="(0.35, 0.75, 0.45)",
                     subsurface_radius=3.0, subsurface_radius_scale="(0.5, 1, 0.6)"),
           marble=MARBLE_MTL, settings=settings("banner", SAMPLES) + "\n" + settings("bannerTest", TEST_SAMPLES)))


def render():
    TMP.mkdir(parents=True, exist_ok=True)
    prefs = TMP / "hprefs"
    prefs.mkdir(exist_ok=True)
    env = dict(os.environ, HOUDINI_USER_PREF_DIR=str(prefs), HOUDINI_PACKAGE_DIR=str(H22 / "houdini"),
               PXR_PLUGINPATH_NAME=str(H22),
               PXR_AR_DEFAULT_SEARCH_PATH=str(HFS / "bin/usd_plugins/hdx/resources/textures"))
    env.pop("PYTHONPATH", None)
    settings = "/Render/bannerTest" if os.environ.get("ELDR_BANNER_TEST") == "1" else "/Render/banner"
    first, count, step = frames()
    cmd = [HFS / "bin" / "husk.exe", "-R", "HdVkRendererPlugin", "-s", settings, "--res", W, H,
           "-f", first, "-n", count, "-i", step, "-o", TMP / "frame.$F4.exr", LAYER]
    print(" ".join(map(str, cmd)), flush=True)
    rc = subprocess.run(list(map(str, cmd)), env=env).returncode
    # husk may fail on exit after writing every frame: the frames decide.
    missing = [f for f in range(first, first + count * step, step) if not (TMP / ("frame.%04d.exr" % f)).exists()]
    if missing:
        sys.exit("husk exited with %d; frames missing: %s" % (rc, missing))


def frames():
    """ELDR_BANNER_FRAMES="first count step" (e.g. "1 4 18": 1, 19, 37, 55), default every frame."""
    f = [int(v) for v in os.environ.get("ELDR_BANNER_FRAMES", "").split()] or [1, FRAMES, 1]
    return f[0], f[1], f[2] if len(f) > 2 else 1


def overlay():
    """The title over a dark band, as an RGBA image in display colours."""
    out = TMP / "overlay.exr"
    # The word in a warm orange (#FF8A1F, display values). No shadow: oiiotool draws it in the
    # complement colour, a light rim around the orange.
    run([OIIO, "--create", "%dx%d" % (W, H), 4,
         "--fill:top=0,0,0,0.45:bottom=0,0,0,0.6", "%dx120+0+0" % W,
         "--fill:top=0,0,0,0.6:bottom=0,0,0,0", "%dx160+0+120" % W,
         "--text:x=640:y=168:size=150:xalign=center:font=segoeuib:color=1,0.541,0.122,1", "Eldr",
         "--text:x=640:y=224:size=28:xalign=center:font=segoeui:shadow=1:color=0.92,0.92,0.92,1",
         "A Vulkan Hydra render delegate for OpenUSD, MaterialX and Houdini Solaris", "-o", out])
    return out


def comp():
    ov = overlay()
    first, count, step = frames()
    for f in range(first, first + count * step, step):
        run([OIIO, ov, "-i", TMP / ("frame.%04d.exr" % f), "--ch", "R,G,B", "--mulc", EXPOSURE,
             '--ociodisplay:from=Linear Rec.709 (sRGB)', "sRGB - Display", "ACES 2.0 - SDR 100 nits (Rec.709)",
             "--ch", "R,G,B,A=1.0", "--over", "--ch", "R,G,B", "-d", "uint8", "-o", TMP / ("comp.%04d.png" % f)])


def webp():
    images = ROOT / "docs" / "images"
    run([OIIO, TMP / "comp.0001.png", "-o:quality=88", images / "banner.jpg"])
    # Houdini's hffmpeg has no animated WebP encoder: Pillow writes it, every frame, 960 wide.
    from PIL import Image
    frames = [Image.open(TMP / ("comp.%04d.png" % f)).convert("RGB") for f in range(1, FRAMES + 1)]
    frames = [im.resize((WEBP_W, round(WEBP_W * im.height / im.width)), Image.LANCZOS) for im in frames]
    frames[0].save(images / "banner.webp", save_all=True, append_images=frames[1:], duration=round(1000 / FPS),
                   loop=0, quality=80, method=6)
    print("banner.webp: %.2f MB" % ((images / "banner.webp").stat().st_size / 1e6))


if __name__ == "__main__":
    steps = sys.argv[1:] or ["layer", "render", "comp", "webp"]
    for step in steps:
        globals()[step]()
