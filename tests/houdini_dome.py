r"""step4 in Houdini: the dome's StinsonBeach.hdr loads through Eldr's package
alone (it adds Houdini's hdx textures folder to the resolver's search path,
as usdenv.cmd does), in Eldr and in Karma, and the Houdini viewport's colour
(houdini:viewport) is Eldr's linear beauty, as Karma's: no tonemap, bloom or
auto exposure of Eldr's own. Houdini only, not part of ctest.
Run: hython tests\houdini_dome.py <build-h22 dir>"""
import os
import subprocess
import sys
import tempfile

import hou
import numpy as np
import OpenImageIO as oiio
from pxr import Gf, Usd, UsdGeom

build = os.path.abspath(sys.argv[1])
scene = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "step4.usda").replace("\\", "/")
work = tempfile.mkdtemp(prefix="eldr_dome_")
usd = os.path.join(work, "dome.usda")
# 16:9 camera; "linear" asks for linear values from Eldr's settings, "viewport"
# leaves Eldr's defaults (aces, auto exposure, bloom) and sets Houdini's flag;
# its half colour var is written straight from Eldr's display output.
with open(usd, "w") as f:
    f.write(f"""#usda 1.0
(
    subLayers = [@{scene}@]
)
over "cam" {{
    float verticalAperture = 11.787
}}
def Scope "Render" {{
    def RenderSettings "linear" {{
        rel camera = </cam>
        int2 resolution = (320, 180)
        custom bool eldr:autoExposure = 0
        custom float eldr:bloomIntensity = 0
        custom string eldr:tonemapper = "none"
        custom int karma:global:pathtracedsamples = 256
    }}
    def RenderSettings "viewport" {{
        rel camera = </cam>
        int2 resolution = (320, 180)
        custom bool houdini:viewport = 1
        rel products = </Render/prod>
    }}
    def RenderProduct "prod" {{
        rel orderedVars = </Render/C>
        token productName = "unused.exr"
    }}
    def RenderVar "C" {{
        uniform string sourceName = "color"
        uniform token sourceType = "raw"
        uniform token dataType = "color4h"
        uniform string driver:parameters:aov:name = "C"
        token driver:parameters:aov:format = "color4h"
    }}
}}
""")

env = dict(os.environ)
env.pop("PXR_AR_DEFAULT_SEARCH_PATH", None)  # the package must add it
env["HOUDINI_USER_PREF_DIR"] = os.path.join(tempfile.mkdtemp(prefix="eldr_pref_"), "houdini__HVER__")
os.makedirs(env["HOUDINI_USER_PREF_DIR"])
env["HOUDINI_PACKAGE_DIR"] = os.path.join(build, "houdini")
husk = os.path.join(hou.text.expandString("$HFS"), "bin", "husk")

stage = Usd.Stage.Open(usd)
cam = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera().frustum
viewProj = cam.ComputeViewMatrix() * cam.ComputeProjectionMatrix()


def render(label, renderer, settings):
    exr = os.path.join(work, label + ".exr")
    subprocess.run([husk, "-R", renderer, "-s", settings, "-o", exr, usd], env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    image = oiio.ImageInput.open(exr)
    spec = image.spec()
    a = np.array(image.read_image("float")).reshape(spec.height, spec.width, spec.nchannels)[..., :3]

    def at(p):  # 7x7 mean around a world point
        n = viewProj.Transform(Gf.Vec3d(*p))
        x, y = int((n[0] * 0.5 + 0.5) * spec.width), int((0.5 - n[1] * 0.5) * spec.height)
        return a[y - 3:y + 4, x - 3:x + 4].reshape(-1, 3).mean(0)
    return {"chromeTop": at((-1.6, 1.55, 2.3)), "floor": a[int(spec.height * 0.88):].reshape(-1, 3).mean(0),
            "mean": a.reshape(-1, 3).mean(0)}


karma = render("karma", "BRAY_HdKarma", "/Render/linear")
linear = render("eldr", "HdVkRendererPlugin", "/Render/linear")
viewport = render("viewport", "HdVkRendererPlugin", "/Render/viewport")
for k, v in (("karma", karma), ("eldr linear", linear), ("eldr viewport", viewport)):
    print("%-14s" % k, "  ".join("%s (%.3f %.3f %.3f)" % (n, *c) for n, c in v.items()))

failed = []
# The chrome ball's top mirrors the sky: blue with the HDR, grey with Eldr's
# white fallback dome, pink with Karma's missing-texture colour.
for k, v in (("karma", karma), ("eldr", linear), ("viewport", viewport)):
    if not v["chromeTop"][2] > 1.5 * v["chromeTop"][0]:
        failed.append(k + ": dome texture not loaded")
if abs(linear["floor"].mean() / karma["floor"].mean() - 1) > 0.08:
    failed.append("eldr floor not within 8% of karma's")
if np.abs(viewport["mean"] / linear["mean"] - 1).max() > 0.01:
    failed.append("viewport colour is not the linear beauty")
print("FAILED: " + "; ".join(failed) if failed else "ok")
sys.exit(1 if failed else 0)
