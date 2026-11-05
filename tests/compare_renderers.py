"""Render one scene with Eldr, Karma and Blender EEVEE and put them side by side.

  hython tests/compare_renderers.py scene.usda outdir [--eldr eldr_default,eldr_final]
         [--karma karma] [--eevee on,off] [--size 640x360] [--force]
  hython tests/compare_renderers.py stats a.exr [ref.exr]

Eldr renders each named RenderSettings prim (/Render/<name>) through husk with HdVkRendererPlugin
from the Houdini build (--build, default <repo>/build-h22l); Karma renders the <karma> prim with
BRAY_HdKarma; EEVEE renders through tests/eevee_render.py with ray tracing on and off, using the
camera and resolution of the Karma prim. Writes <name>.exr per image, sheet.png (a labelled
mosaic, 3 per row) and stats.txt: per image the mean luminance, the ratio to Karma, and the mean
absolute difference to Karma (relative to Karma's mean luminance) as rendered and after scaling
the image to Karma's mean luminance (exposure-matched). Existing EXRs are reused unless --force.
Karma gets a layer that shows domes to its camera (Karma's default hides them; Eldr and EEVEE
show them) unless the scene authors Render Light Geometry or --karma-dome-hidden is given.
A scene without lights (lit by emissive surfaces alone, e.g. a Cornell box) renders Karma Path
Traced at EMISSIVE_SAMPLES samples (Variance sampling leaves it noisy), unless --karma-samples
says otherwise.
Run it with hython (it reads the stage with pxr); it needs only Houdini and Blender.
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
HFS = os.environ.get("HFS", "C:/Program Files/Side Effects Software/Houdini 22.0.466")
BLENDER = os.environ.get("BLENDER", "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe")
HUSK = os.path.join(HFS, "bin", "husk.exe")
HOIIO = os.path.join(HFS, "bin", "hoiiotool.exe")
# Karma's path-traced samples for scenes lit only by emissive surfaces.
EMISSIVE_SAMPLES = 2048


def oiio(*args):
    return subprocess.run([HOIIO, *args], capture_output=True, text=True).stdout


def avg(*args):
    """Mean R, G, B of the image hoiiotool leaves on top of the stack after `args`."""
    m = re.search(r"Stats Avg: ([-\d.e+naif ]+)", oiio(*args, "--printstats"))
    return [float(v) for v in m.group(1).split()[:3]] if m else None


def lum(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def stats(path, ref=None):
    """(mean luminance, ratio to ref, mean abs diff / ref lum, same exposure-matched)."""
    a = avg(path, "--ch", "R,G,B")
    if a is None:
        return None
    if ref is None or not os.path.exists(ref):
        return lum(a), None, None, None
    r = avg(ref, "--ch", "R,G,B")
    lr, la = lum(r), lum(a)
    if lr <= 0 or la <= 0:
        return la, None, None, None
    d = avg(path, "--ch", "R,G,B", ref, "--ch", "R,G,B", "--absdiff")
    s = lr / la
    dm = avg(path, "--ch", "R,G,B", "--mulc", "%g,%g,%g" % (s, s, s), ref, "--ch", "R,G,B", "--absdiff")
    return la, la / lr, sum(d) / 3 / lr, sum(dm) / 3 / lr


def settings_prims(stage):
    """{name: path} of the stage's RenderSettings prims."""
    return {p.GetName(): str(p.GetPath()) for p in stage.Traverse() if p.GetTypeName() == "RenderSettings"}


def has_lights(stage):
    """Whether the stage has a UsdLux light (a dome, distant, rect, mesh light...)."""
    from pxr import UsdLux
    return any(p.HasAPI(UsdLux.LightAPI) for p in stage.Traverse())


def karma_layer(stage, usd, path, show_domes, settings, samples):
    """A layer over `usd` for Karma: shows its domes to the camera, as Eldr and EEVEE do, unless
    the scene authors Render Light Geometry (Karma's schema default hides them: a black
    background), renders the beauty only and sets the samples."""
    from pxr import Sdf
    domes = [p for p in stage.Traverse() if show_domes and p.GetTypeName() in ("DomeLight", "DomeLight_1")
             and not any(p.GetAttribute("inputs:%s:light:renderlightgeo" % ns).HasAuthoredValue()
                         for ns in ("eldr", "karma"))]
    layer = Sdf.Layer.CreateAnonymous(".usda")
    layer.subLayerPaths.append(usd.replace("\\", "/"))
    for d in domes:
        spec = Sdf.CreatePrimInLayer(layer, d.GetPath())
        spec.specifier = Sdf.SpecifierOver
        Sdf.AttributeSpec(spec, "inputs:karma:light:renderlightgeo", Sdf.ValueTypeNames.Bool).default = True
    # Beauty only: a scene's own products may carry render vars Karma can't make (a black image).
    spec = Sdf.CreatePrimInLayer(layer, settings)
    spec.specifier = Sdf.SpecifierOver
    Sdf.RelationshipSpec(spec, "products", custom=False).targetPathList.ClearEditsAndMakeExplicit()
    if samples:  # Karma reads pathtracedsamples only in Path Traced mode (its default is Variance)
        Sdf.AttributeSpec(spec, "karma:global:convergence_mode", Sdf.ValueTypeNames.Token).default = "Path Traced"
        Sdf.AttributeSpec(spec, "karma:global:pathtracedsamples", Sdf.ValueTypeNames.Int).default = samples
    layer.Export(path)
    return path


def run(cmd, log, env=None):
    t = time.time()
    with open(log, "w") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env).returncode
    return code, time.time() - t


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "stats":
        s = stats(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
        print("lum %.4f ratio %s mad %s madExp %s" % (s[0], *("%.3f" % v if v is not None else "-" for v in s[1:])))
        return
    p = argparse.ArgumentParser()
    p.add_argument("usd")
    p.add_argument("outdir")
    p.add_argument("--eldr", default="eldr_default,eldr_final")
    p.add_argument("--karma", default="karma")
    p.add_argument("--eevee", default="on,off")
    p.add_argument("--size")
    p.add_argument("--build", default=os.path.join(REPO, "build-h22l"))
    p.add_argument("--force", action="store_true")
    p.add_argument("--karma-samples", type=int,
                   help="Karma's path-traced samples (default: %d without lights, else the scene's)" % EMISSIVE_SAMPLES)
    p.add_argument("--karma-dome-hidden", action="store_true", help="keep Karma's default: domes hidden from the camera")
    a = p.parse_args()
    usd = os.path.abspath(a.usd)
    os.makedirs(a.outdir, exist_ok=True)
    from pxr import Usd
    stage = Usd.Stage.Open(usd)
    prims = settings_prims(stage)
    if not prims:
        sys.exit("no RenderSettings prim in %s" % usd)
    karma = prims.get(a.karma) or next(iter(prims.values()))
    if a.karma_samples is None and not has_lights(stage):
        a.karma_samples = EMISSIVE_SAMPLES
        print("no lights: Karma path traced at %d samples" % EMISSIVE_SAMPLES, flush=True)
    # The stage's start frame, as Houdini's ROPs render it.
    frame = "%g" % (stage.GetStartTimeCode() if stage.HasAuthoredTimeCodeRange() else 1)
    kusd = karma_layer(stage, usd, os.path.join(os.path.abspath(a.outdir), "karma_layer.usda"),
                       not a.karma_dome_hidden, karma, a.karma_samples)
    res = ["--res", *a.size.split("x")] if a.size else []

    env = dict(os.environ, MSYS_NO_PATHCONV="1", HOUDINI_PACKAGE_DIR=os.path.join(a.build, "houdini"),
               PXR_PLUGINPATH_NAME=a.build)
    env.setdefault("HOUDINI_USER_PREF_DIR", os.path.join(a.outdir, "hprefs__HVER__"))
    jobs = [("karma", ["-R", "BRAY_HdKarma", "-s", karma], kusd)]
    jobs += [(n, ["-R", "HdVkRendererPlugin", "-s", prims[n]], usd) for n in a.eldr.split(",") if n in prims]
    # Blender with its own colour config, not Houdini's $OCIO.
    blender_env = {k: v for k, v in os.environ.items() if k != "OCIO"}
    images = []
    for name, args, scene in jobs:
        out = os.path.join(a.outdir, name + ".exr")
        if a.force or not os.path.exists(out):
            code, sec = run([HUSK, *args, *res, "-f", frame, "-n", "1", "-o", out, scene], out[:-4] + ".log", env)
            print("%s: %s %.1f s" % (name, "ok" if os.path.exists(out) else "FAILED (%d)" % code, sec), flush=True)
        images.append((name, out))
    for rt in a.eevee.split(","):
        if not rt:
            continue
        name = "eevee_rt" + rt
        out = os.path.join(a.outdir, name + ".exr")
        if a.force or not os.path.exists(out):
            size = ["--size", a.size] if a.size else []
            code, sec = run([BLENDER, "-b", "--factory-startup", "-P", os.path.join(HERE, "eevee_render.py"), "--",
                             usd, out, "--settings", karma, "--frame", frame, "--rt", rt, *size], out[:-4] + ".log", blender_env)
            print("%s: %s %.1f s" % (name, "ok" if os.path.exists(out) else "FAILED (%d)" % code, sec), flush=True)
        images.append((name, out))

    ref = os.path.join(a.outdir, "karma.exr")
    lines = ["%-14s %8s %7s %7s %7s" % ("image", "lum", "/karma", "mad", "madExp")]
    sheet, n = [], 0
    for name, path in images:
        if not os.path.exists(path):
            lines.append("%-14s missing" % name)
            continue
        s = stats(path, ref if name != "karma" else None)
        f = ["%.3f" % v if v is not None else "-" for v in s[1:]] if s else ["-"] * 3
        lines.append("%-14s %8.4f %7s %7s %7s" % (name, s[0] if s else 0, *f))
        label = name if not s or s[1] is None else "%s  x%.2f  d%.2f/%.2f" % (name, s[1], s[2], s[3])
        sheet += [path, "--ch", "R,G,B", "--powc", "0.4545,0.4545,0.4545",
                  "--text:x=8:y=22:size=18:color=1,1,0", label]
        n += 1
    with open(os.path.join(a.outdir, "stats.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    if n:
        cols = min(3, n)
        rows = (n + cols - 1) // cols
        while n < cols * rows:  # pad the last row with black
            sheet += [ref, "--ch", "R,G,B", "--mulc", "0,0,0"]
            n += 1
        oiio(*sheet, "--mosaic", "%dx%d" % (cols, rows), "--clamp:min=0:max=1", "-d", "uint8",
             "-o", os.path.join(a.outdir, "sheet.png"))


if __name__ == "__main__":
    main()
