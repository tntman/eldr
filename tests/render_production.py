"""Render the production scenes (tests/scenes/production: Intel Sponza with its add-ons, Jungle Ruins) with
Eldr default, Eldr final, Karma XPU and Karma CPU and write the Production scenes section of docs/gallery.md
and its cards in docs/images/gallery/production/.

  python tests/render_production.py [outdir] [--only sponza_atrium,jungle] [--force] [--build <repo>/build-h22l]

The scenes' packages aren't in the repository: the layers sublayer them from E:/usd-testscenes. A view whose
files are missing is skipped (and keeps its last results). Each view is one layer, rendered through husk at
960 x 540 with its /Render/<renderer> settings prim (tests/scenes/production/settings.usda). Renders land in
outdir (default %TEMP%/eldr_production) and are reused unless --force. Times are husk wall clock (startup and
scene load included); for Eldr also its time in the render pass (HDVK_TIMING=2) and the peak GPU memory the
render added (nvidia-smi). Brightness is the mean luminance against Karma CPU's (compare_renderers.stats).
render_gallery.py appends section() to docs/gallery.md; this script splices it in by itself.
"""
import argparse
import csv as csvfile
import json
import os
import re
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from compare_renderers import HUSK, oiio, run, stats  # noqa: E402

SCENES = os.path.join(HERE, "scenes", "production")
IMAGES = os.path.join(REPO, "docs", "images", "gallery", "production")
RESULTS = os.path.join(IMAGES, "production.json")
HEADING = "## Production scenes"
# (layer, heading, what the view shows)
VIEWS = [
    ("sponza_atrium", "Sponza atrium: sun, sky and bounced light",
     "The courtyard with the curtains, ivy and cypress add-ons: sun-lit stone, sky through the open roof and "
     "light bounced into the arcades (15 million triangles)."),
    ("sponza_arcade", "Sponza arcade: GI through curtains",
     "The ground-floor arcade lit mostly by sunlight bounced off the courtyard floor and through the curtains."),
    ("sponza_candles", "10,000 candles: many lights",
     "Sponza at night lit by 10,000 small sphere lights, one per candle flame, and a faint sky."),
    ("sponza_knight", "Knight: animated mesh, motion blur",
     "The knight add-on mid-lunge: deforming meshes from point time samples, blurred over a 180 degree shutter."),
    ("sponza_flood", "Flood: water refraction",
     "A FLIP water mesh (1.2 million faces) pouring along the courtyard, in clear water refracting the floor."),
    ("sponza_explosion", "Explosion: volumes",
     "Two pyro simulations (OpenVDB density and temperature): dark smoke lit by the sun, fire glowing by "
     "temperature."),
    ("jungle", "Jungle Ruins: dense instanced foliage",
     "About 170 million instanced triangles of plants around a pyramid, lit by the sky dome alone."),
    ("jungle_close", "Jungle Ruins, closer",
     "The same camera zoomed in 2.4 times onto the plants and stones."),
    ("crowd", "Crowd: 9,324 skinned agents",
     "9,324 UsdSkel agents (20.7 million triangles) skinned on the GPU, at 1280 x 720. Eldr only."),
]
# Views that render with some renderers only, and the environment they need.
ONLY = {"crowd": (("eldr_default", "eldr_final"), {"HD_ENABLE_DEFERRED_SKINNING": "1"})}
RENDERERS = [("eldr_default", "Eldr default", "HdVkRendererPlugin"), ("eldr_final", "Eldr final", "HdVkRendererPlugin"),
             ("karma_xpu", "Karma XPU", "BRAY_HdKarmaXPU"), ("karma_cpu", "Karma CPU", "BRAY_HdKarma")]
CREDIT = ("Intel Sponza (Frank Meinl, Anton Kaplanyan, Intel Sample Library, 2022) and add-ons by the Sponza Add-on "
          "Package Crew (Katica Putica, Cristiano Siqueira, Timothy Heath, Justin Prazen, Sebastian Herholz, Bruce "
          "Cherniak, Anton Kaplanyan); Jungle Ruins (Cristiano Siqueira, Charlene Teets, Sebastian Herholz, Anton "
          "Sochenov, Anton Kaplanyan, Intel Sample Library, 2024). CC BY 4.0.")


def external_files(layer, seen=None):
    """The absolute paths a production layer (and the repo layers it sublayers) names."""
    seen = set() if seen is None else seen
    out = []
    for ref in re.findall(r"@([^@]+)@", open(layer, encoding="utf-8").read()):
        if re.match(r"[A-Za-z]:/", ref):
            out.append(ref)
        else:
            p = os.path.normpath(os.path.join(os.path.dirname(layer), ref))
            if p.endswith(".usda") and p.startswith(SCENES) and p not in seen:
                seen.add(p)
                out += external_files(p, seen)
    return out


def gpu_used_mb():
    try:
        r = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                           capture_output=True, text=True)
        return int(r.stdout.split()[0])
    except (OSError, ValueError, IndexError):
        return None


def husk(cmd, log, env):
    """run() while sampling the GPU's used memory: (code, seconds, peak MB above the start)."""
    base, peak, done = gpu_used_mb(), [0], threading.Event()

    def poll():
        while not done.wait(0.5):
            m = gpu_used_mb()
            if m is not None:
                peak[0] = max(peak[0], m)
    t = threading.Thread(target=poll)
    t.start()
    code, sec = run(cmd, log, env)
    done.set()
    t.join()
    return code, sec, (peak[0] - base) if base is not None and peak[0] else None


def render(view, outdir, build, force):
    """{renderer: {exr, sec, ex, mem}} for one view; renders what isn't there yet."""
    d = os.path.join(outdir, view)
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, MSYS_NO_PATHCONV="1", HOUDINI_PACKAGE_DIR=os.path.join(build, "houdini"),
               PXR_PLUGINPATH_NAME=build, HDVK_TIMING="2")
    env.setdefault("HOUDINI_USER_PREF_DIR", os.path.join(outdir, "hprefs__HVER__"))
    times = os.path.join(d, "times.json")
    known = json.load(open(times)) if os.path.exists(times) else {}
    layer = os.path.join(SCENES, view + ".usda")
    names, extra = ONLY.get(view, ([r for r, _, _ in RENDERERS], {}))
    env.update(extra)
    for r, _, plugin in RENDERERS:
        if r not in names:
            continue
        exr, log, csv = (os.path.join(d, r + ext) for ext in (".exr", ".log", ".csv"))
        if force or not os.path.exists(exr) or r not in known:
            if os.path.exists(csv):
                os.remove(csv)
            code, sec, mem = husk([HUSK, "-R", plugin, "-s", "/Render/" + r, "-f", "1", "-n", "1", "-o", exr, layer],
                                  log, dict(env, HDVK_TIMING_CSV=csv))
            rows = list(csvfile.DictReader(open(csv))) if os.path.exists(csv) else []
            known[r] = {"sec": sec, "ex": sum(float(x["execute_ms"]) for x in rows) / 1000 if rows else None,
                        "mem": mem if plugin == "HdVkRendererPlugin" else None}
            print("%-16s %-13s %s %.1f s, %s MB" % (view, r, "ok" if os.path.exists(exr) else "FAILED (%d)" % code,
                                                    sec, mem), flush=True)
            json.dump(known, open(times, "w"))
        known[r]["exr"] = exr
    return {r: known[r] for r in names}


def card(exr, jpg):
    """A 960 x 540 sRGB JPEG of a render."""
    oiio(exr, "--ch", "R,G,B", "--croptofull", "--resize", "960x0", "--clamp:min=0", "--colorconvert", "linear",
         "sRGB", "--clamp:min=0:max=1", "-d", "uint8", "--attrib", "Compression", "jpeg:85", "-o", jpg)


def section(results=None):
    """The Production scenes section of docs/gallery.md, as lines; [] before the first render."""
    if results is None:
        results = json.load(open(RESULTS)) if os.path.exists(RESULTS) else {}
    if not results:
        return []
    md = [HEADING, "",
          "Intel's Sponza (with its curtains, ivy, cypress, candles, knight, flood and explosion add-ons) and Jungle "
          "Ruins, rendered with husk at 960 x 540: *Eldr default* (every effect without ray tracing: SSGI, ambient "
          "occlusion, SSR, screen-space refraction, subsurface, soft shadows, denoising, 256 film samples), *Eldr "
          "final* (ray-traced reflections, refraction and shadows, GI, final gather, denoising, 256 film samples), "
          "Karma XPU (256 path-traced samples) and Karma CPU (128), both with 4 diffuse bounces and OIDN. The "
          "layers are in `tests/scenes/production`; the packages come from the Intel Sample Library. "
          "`python tests/render_production.py` renders them again. Times are husk wall clock, scene load included; "
          "for Eldr, in brackets, its time in the render pass, the rest (husk startup, scene load and sync) and "
          "the GPU memory the render took. Brightness is the mean luminance against Karma CPU's.", ""]
    for view, heading, what in VIEWS:
        r = results.get(view)
        if not r:
            continue
        md += ["### " + heading, "", what, ""]
        for k, label, _ in RENDERERS:
            x = r.get(k)
            if not x or not x.get("jpg"):
                continue
            t = "%s: %.0f s" % (label, x["sec"])
            if x.get("ex") is not None:
                t += " (%.1f s in the render pass, %.0f s loading%s)" % (
                    x["ex"], x["sec"] - x["ex"], ", %.1f GB GPU" % (x["mem"] / 1024) if x.get("mem") else "")
            if k != "karma_cpu" and x.get("ratio") is not None:
                t += ", brightness x%.2f" % x["ratio"]
            md.append("![%s](images/gallery/production/%s)" % (t, x["jpg"]))
        md.append("")
    return md + ["*" + CREDIT + "*", ""]


def splice(lines):
    """Replace docs/gallery.md's Production scenes section (the end of the page) with lines."""
    path = os.path.join(REPO, "docs", "gallery.md")
    text = open(path, encoding="utf-8").read()
    i = text.find(HEADING)
    text = (text[:i] if i >= 0 else text.rstrip("\n") + "\n\n") + "\n".join(lines)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("outdir", nargs="?", default=os.path.join(tempfile.gettempdir(), "eldr_production"))
    p.add_argument("--only")
    p.add_argument("--force", action="store_true")
    p.add_argument("--build", default=os.path.join(REPO, "build-h22l"))
    a = p.parse_args()
    os.makedirs(IMAGES, exist_ok=True)
    results = json.load(open(RESULTS)) if os.path.exists(RESULTS) else {}
    for view, _, _ in VIEWS:
        if a.only and view not in a.only.split(","):
            continue
        missing = [f for f in external_files(os.path.join(SCENES, view + ".usda")) if not os.path.exists(f)]
        if missing:
            print("%s: skipped, missing %s" % (view, missing[0]))
            continue
        rs = render(view, a.outdir, a.build, a.force)
        ref = rs["karma_cpu"]["exr"] if "karma_cpu" in rs else None
        out = {}
        for k in rs:
            x = dict(rs[k])
            if os.path.exists(x["exr"]):
                x["jpg"] = "%s_%s.jpg" % (view, k)
                card(x["exr"], os.path.join(IMAGES, x["jpg"]))
                st = stats(x["exr"], ref if k != "karma_cpu" else None)
                x["ratio"] = st[1] if st else None
            del x["exr"]
            out[k] = x
        results[view] = out
        json.dump(results, open(RESULTS, "w"), indent=1)
    splice(section(results))


if __name__ == "__main__":
    main()
