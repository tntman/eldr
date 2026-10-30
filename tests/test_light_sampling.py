"""Light sampling (eldr:rtLightSampling) and gather roulette (eldr:finalGatherRoulette) keep the converged
film: the Cornell box lit by seven shadowed lights (over four, so the per-pixel shadows share the samples;
one a spot, so some pixels cull it) with traced shadows, GI and the final gather. The image's mean with
both on is within 1% of both off, and so is each quarter of it.
Run: usdenv.cmd python tests\\test_light_sampling.py <exrinfo>"""
import os
import subprocess
import sys

from final_settings import FINAL_ARGS
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
exrinfo = sys.argv[1]
folder = tempfile.mkdtemp(prefix="eldr_light_sampling_")
exr = os.path.join(folder, "beauty.exr").replace("\\", "/")
scene = os.path.join(folder, "scene.usda")
SIZE = 128
LIGHTS = [((-0.6, 1.7, -0.5), (1.0, 0.5, 0.3)), ((0.0, 1.7, -0.5), (0.4, 1.0, 0.5)), ((0.6, 1.7, -0.5), (0.4, 0.6, 1.0)),
          ((-0.6, 1.7, 0.4), (1.0, 1.0, 0.6)), ((0.0, 1.7, 0.4), (1.0, 0.8, 1.0)), ((0.6, 1.7, 0.4), (0.7, 1.0, 1.0))]
with open(scene, "w") as f:
    f.write(f"""#usda 1.0
(
    subLayers = [@{os.path.join(HERE, "scenes", "cornell.usda").replace(os.sep, "/")}@]
)
over RectLight "light" {{
    float inputs:intensity = 0
}}
""" + "".join(f"""def SphereLight "l{i}" {{
    float inputs:intensity = 4
    color3f inputs:color = {c}
    float inputs:radius = 0.08
    double3 xformOp:translate = {p}
    uniform token[] xformOpOrder = ["xformOp:translate"]
}}
""" for i, (p, c) in enumerate(LIGHTS)) + f"""def SphereLight "spot" (prepend apiSchemas = ["ShapingAPI"]) {{
    float inputs:intensity = 30
    float inputs:radius = 0.05
    float inputs:shaping:cone:angle = 20
    double3 xformOp:translate = (0.3, 1.9, 0.2)
    float xformOp:rotateX = -90
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
}}
def RenderSettings "Render" {{
    rel camera = </cam>
    rel products = </Render/product>
    def RenderProduct "product" {{
        uniform token productName = "{exr}"
        rel camera = </cam>
        int2 resolution = ({SIZE}, {SIZE})
        rel orderedVars = [</Render/C>]
    }}
    def RenderVar "C" {{
        uniform string sourceName = "C.*[LO]"
        uniform token sourceType = "lpe"
        uniform token dataType = "color4f"
    }}
}}
""")


def render(on):
    if os.path.exists(exr):
        os.remove(exr)
    subprocess.run([sys.executable, os.path.join(HERE, "exr_record.py"), scene, "/Render", "/cam",
                    *FINAL_ARGS, "eldr:filmSamples=128", "eldr:autoExposure=false",
                    "eldr:rtShadows=1", "eldr:gi=true", "eldr:finalGather=true",
                    "eldr:rtLightSampling=%s" % on, "eldr:finalGatherRoulette=%s" % on],
                   check=True, timeout=900, env=dict(os.environ, EXR_SIZE=str(SIZE)))
    out = subprocess.run([exrinfo, exr, "--grid", "2", "2"], capture_output=True, text=True,
                         check=True, timeout=120).stdout.splitlines()
    chans = [l.split()[1] for l in out if l.startswith("channel ")]
    cells = [dict(zip(chans, map(float, l.split()[3:]))) for l in out if l.startswith("cell ")]
    return [sum(c.get(k, c.get("C." + k, 0.0)) for k in ("R", "G", "B")) / 3.0 for c in cells]


failures = []


def expect(ok, what):
    print(("ok  " if ok else "FAIL") + " " + what)
    if not ok:
        failures.append(what)


on, off = render("true"), render("false")
mean_on, mean_off = sum(on) / len(on), sum(off) / len(off)
expect(mean_off > 0.0 and abs(mean_on - mean_off) <= 0.01 * mean_off,
       "image mean %.5f with light sampling, %.5f without" % (mean_on, mean_off))
for i, (a, b) in enumerate(zip(on, off)):
    expect(abs(a - b) <= 0.01 * max(b, 1e-6), "quarter %d: %.5f against %.5f" % (i, a, b))
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
