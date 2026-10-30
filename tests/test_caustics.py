"""Caustics (eldr:caustics): tests/scenes/caustics.usda's glass ball focuses the sun onto the floor.
With caustics the `caustics` AOV holds a patch several times brighter than the sunlit floor, it is part
of indirect diffuse, and the splits still add up to the beauty there; off, the AOV is black. The ball
blocks the sun's direct light under the patch (its light arrives as photons), also with
eldr:translucentShadows and eldr:rtTranslucentShadows off (no double counting).
Run: usdenv.cmd python tests\\test_caustics.py <exrinfo>"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
exrinfo = sys.argv[1]
folder = tempfile.mkdtemp(prefix="eldr_caustics_")
exr = os.path.join(folder, "caustics.exr").replace("\\", "/")
scene = os.path.join(folder, "scene.usda")
SIZE, CELLS = 128, 16
VARS = [("C", "C.*[LO]", "lpe", "color4f"), ("caustics", "caustics", "raw", "color3f"),
        ("directdiffuse", "C<RD>L", "lpe", "color3f"), ("indirectdiffuse", "C<RD>.+L", "lpe", "color3f"),
        ("directglossy", "C<RG>L", "lpe", "color3f"), ("indirectglossy", "C<RG>.+L", "lpe", "color3f")]
with open(scene, "w") as f:
    f.write(f"""#usda 1.0
(
    subLayers = [@{os.path.join(HERE, "scenes", "caustics.usda").replace(os.sep, "/")}@]
)
def RenderSettings "Render" {{
    rel camera = </cam>
    rel products = </Render/product>
    def RenderProduct "product" {{
        uniform token productName = "{exr}"
        rel camera = </cam>
        int2 resolution = ({SIZE}, {SIZE})
        rel orderedVars = [{", ".join(f"</Render/{n}>" for n, *_ in VARS)}]
    }}
""" + "".join(f"""    def RenderVar "{n}" {{
        uniform string sourceName = "{s}"
        uniform token sourceType = "{t}"
        uniform token dataType = "{d}"
    }}
""" for n, s, t, d in VARS) + "}\n")


def render(caustics, *extra):
    if os.path.exists(exr):
        os.remove(exr)
    subprocess.run([sys.executable, os.path.join(HERE, "exr_record.py"), scene, "/Render", "/cam",
                    "eldr:finalQuality=true", "eldr:filmSamples=16", "eldr:autoExposure=false",
                    "eldr:caustics=%d" % caustics, *extra], check=True, timeout=600, env=dict(os.environ, EXR_SIZE=str(SIZE)))
    out = subprocess.run([exrinfo, exr, "--grid", str(CELLS), str(CELLS)], capture_output=True, text=True,
                         check=True, timeout=120).stdout.splitlines()
    chans = [l.split()[1] for l in out if l.startswith("channel ")]
    return [dict(zip(chans, map(float, l.split()[3:]))) for l in out if l.startswith("cell ")]


failures = []


def expect(ok, what):
    print(("ok  " if ok else "FAIL") + " " + what)
    if not ok:
        failures.append(what)


on, off = render(2), render(0)
g = lambda cell, var: cell.get(var + ".G", cell.get("G", 0.0) if var == "C" else 0.0)
best = max(range(len(on)), key=lambda i: g(on[i], "caustics"))
lit = max(g(c, "directdiffuse") for c in off)  # the sunlit floor
expect(g(on[best], "caustics") > 2.0 * lit,
       "caustic patch %.3f against the sunlit floor %.3f" % (g(on[best], "caustics"), lit))
expect(g(on[best], "indirectdiffuse") >= 0.99 * g(on[best], "caustics"), "the caustics are indirect diffuse")
splits = sum(g(on[best], v) for v in ("directdiffuse", "indirectdiffuse", "directglossy", "indirectglossy"))
expect(abs(g(on[best], "C") - splits) <= 0.03 * g(on[best], "C"),
       "splits %.3f add up to the beauty %.3f at the patch" % (splits, g(on[best], "C")))
expect(g(on[best], "C") > g(off[best], "C") + 0.5 * g(on[best], "caustics"),
       "the beauty brightens there with caustics (%.3f, off %.3f)" % (g(on[best], "C"), g(off[best], "C")))
expect(max(g(c, "caustics") for c in off) == 0.0, "off: no caustics")
untinted = render(2, "eldr:translucentShadows=false", "eldr:rtTranslucentShadows=false")
for name, r in (("on", on), ("tint settings off", untinted)):
    expect(g(r[best], "directdiffuse") <= 0.1 * lit,
           "%s: the ball blocks the sun under the patch (direct %.3f, sunlit %.3f)" % (name, g(r[best], "directdiffuse"), lit))
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
