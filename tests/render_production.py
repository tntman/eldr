"""Render the production scenes (tests/scenes/production: Intel Sponza with its add-ons, a crowd)
with Eldr (raster + SSGI, and ray traced), Karma XPU and Karma CPU and write the Production scenes section of
docs/gallery.md and its pictures in docs/images/gallery/production/.

  python tests/render_production.py [outdir] [--only sponza_atrium,crowd] [--force] [--build <repo>/build-h22l]

The scenes' packages aren't in the repository: the layers sublayer them from E:/usd-testscenes. A view whose
files are missing is skipped (and keeps its last results). Each view is one layer, rendered through husk with
its /Render/<renderer> settings prim (tests/scenes/production/settings.usda). Renders land in outdir (default
%TEMP%/eldr_production) and are reused unless --force.
Times: render_gallery.timed, as the gallery: Eldr renders twice and times the second (warm pipeline cache);
render and load from each renderer's own log. Eldr's GPU memory is the peak nvidia-smi
reports during the render less what was in use before it. Brightness is the mean luminance against Karma
CPU's (compare_renderers.stats). render_gallery.py appends section() to docs/gallery.md; this script
splices it in by itself.
"""
import argparse
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
from render_gallery import CARD, COLOURS, GLYPH, duration, hardware, hexrgb, timed  # noqa: E402

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
     "Sponza at night lit by 10,000 small sphere lights, one per candle flame, and a faint sky; the camera's "
     "exposure is -2.5."),
    ("sponza_knight", "Knight: animated mesh, motion blur",
     "The knight add-on mid-lunge: deforming meshes from point time samples, blurred over a 180 degree shutter."),
    ("sponza_flood", "Flood: water refraction",
     "A FLIP water mesh (1.2 million faces) pouring along the courtyard, in clear water refracting the floor."),
    ("sponza_explosion", "Explosion: volumes",
     "Two pyro simulations (OpenVDB density and temperature): dark smoke lit by the sun, fire glowing by "
     "temperature."),
    ("crowd", "Crowd: 9,324 skinned agents",
     "9,324 UsdSkel agents (20.7 million triangles) skinned on the GPU, at 1280 x 720. Eldr only."),
]
# Views that render with some renderers only, and the environment they need.
ONLY = {"crowd": (("eldr_default", "eldr_final"), {"HD_ENABLE_DEFERRED_SKINNING": "1"})}
# (settings prim, card name, effects line, husk delegate, render_gallery colour key and glyph)
RENDERERS = [
    ("eldr_default", "Eldr raster + SSGI", "SSGI, SSR, AO, screen-space refraction, subsurface, soft shadows, denoise",
     "HdVkRendererPlugin", "circle"),
    ("eldr_final", "Eldr ray traced", "Traced reflections, refraction, shadows, GI, final gather, caustics, denoise",
     "HdVkRendererPlugin", "diamond"),
    ("karma_xpu", "Karma XPU", "Path traced, 4 diffuse bounces, OIDN", "BRAY_HdKarmaXPU", "triangle"),
    ("karma_cpu", "Karma CPU", "Path traced, 4 diffuse bounces, OIDN", "BRAY_HdKarma", "square"),
]
LABEL = {r[0]: r[1] for r in RENDERERS}
KARMA_LIMIT = 600  # seconds: husk --timelimit for Karma (its card says "time-limited" when it stops early)
CREDIT = ("Intel Sponza (Frank Meinl, Anton Kaplanyan, Intel Sample Library, 2022) and add-ons by the Sponza Add-on "
          "Package Crew (Katica Putica, Cristiano Siqueira, Timothy Heath, Justin Prazen, Sebastian Herholz, Bruce "
          "Cherniak, Anton Kaplanyan). CC BY 4.0.")


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


def resolution(layer):
    m = re.search(r"resolution = \((\d+), (\d+)\)", open(layer, encoding="utf-8").read())
    return (int(m.group(1)), int(m.group(2))) if m else (960, 540)


def samples(layer, r):
    if r.startswith("eldr"):
        # A view's own `over "<prim>"` may set fewer film samples.
        m = re.search(r'over "%s"\s*\{[^}]*eldr:filmSamples = (\d+)' % r, open(layer, encoding="utf-8").read())
        return "%s film samples" % (m.group(1) if m else 256)
    t = open(os.path.join(SCENES, "settings.usda"), encoding="utf-8").read()
    return "%s spp" % re.search(r'"%s".*?pathtracedsamples = (\d+)' % r, t, re.S).group(1)


def render(view, outdir, build, force):
    """{renderer: {exr, sec, wall, mem}} for one view; renders what isn't there yet."""
    d = os.path.join(outdir, view)
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, MSYS_NO_PATHCONV="1", HOUDINI_PACKAGE_DIR=os.path.join(build, "houdini"),
               PXR_PLUGINPATH_NAME=build, HDVK_TIMING="2",
               # The add-ons bind materials without applying MaterialBindingAPI, which USD ignores by default.
               USD_SHADE_MATERIAL_BINDING_API_CHECK="allowMissingAPI")
    env.setdefault("HOUDINI_USER_PREF_DIR", os.path.join(outdir, "hprefs__HVER__"))
    times = os.path.join(d, "times.json")
    known = json.load(open(times)) if os.path.exists(times) else {}
    layer = os.path.join(SCENES, view + ".usda")
    names, extra = ONLY.get(view, ([r[0] for r in RENDERERS], {}))
    env.update(extra)
    out = {}
    for r, _, _, plugin, _ in RENDERERS:
        if r not in names:
            continue
        exr, log, csv = (os.path.join(d, r + ext) for ext in (".exr", ".log", ".csv"))
        if force or r not in known:
            for f in (exr, csv):
                if os.path.exists(f):
                    os.remove(f)
            for attempt in range(2):  # ponytail: one retry for an intermittent crash in Eldr's parallel mesh sync
                (code, wall, mem), sec, load, cold = timed(
                    [HUSK, "-R", plugin, "-s", "/Render/" + r, "-f", "1", "-n", "1", "-V", "2"] +
                    ([] if plugin == "HdVkRendererPlugin" else ["--timelimit", str(KARMA_LIMIT)]) + ["-o", exr, layer],
                    log, csv, env, husk)
                if os.path.exists(exr) or "Fatal error" not in open(log, errors="replace").read():
                    break
                os.replace(log, log[:-4] + "_crash.log")
                if os.path.exists(csv):
                    os.remove(csv)
            known[r] = dict(ok=os.path.exists(exr), wall=wall, sec=sec if sec is not None else wall, load=load,
                            cold=cold,
                            mem=mem if plugin == "HdVkRendererPlugin" else None,
                            limited=os.path.exists(log) and
                            "Run time %d exceeded" % KARMA_LIMIT in open(log, errors="replace").read())
            print("%-16s %-13s %s render %.1f s, load %s, cold %s, wall %.1f s, %s MB" % (
                view, r, "ok" if known[r]["ok"] else "FAILED (%d)" % code, known[r]["sec"], load, cold, wall, mem),
                flush=True)
            json.dump(known, open(times, "w"))
        if known.get(r, {}).get("ok"):
            out[r] = dict(known[r], exr=exr)
    return out


def info(x, r, layer):
    """A card's lines under its name: resolution, samples, times; brightness and GPU memory."""
    spp = samples(layer, r)
    if x.get("limited"):
        spp = "≤ " + spp + ", time-limited"
    line1 = "%d × %d · %s · %s render · %s load" % (*resolution(layer), spp, duration(x["sec"]),
                                                   duration(x["load"] if x.get("load") is not None
                                                            else max(0, x["wall"] - x["sec"])))
    line2 = "Brightness reference" if r == "karma_cpu" else (
        "Brightness %.2f× Karma CPU" % x["ratio"] if x.get("ratio") is not None else "")
    if x.get("mem"):
        line2 += (" · " if line2 else "") + "GPU %.1f GB" % (x["mem"] / 1024)
    return line1, line2


def panel(view, rs, path):
    """The view's cards (960 wide renders with their renderer, effects, times) as one JPEG, two per row."""
    layer = os.path.join(SCENES, view + ".usda")
    rw, rh = resolution(layer)
    w, h = 960, round(960 * rh / rw)
    cw, base = w + 16, 12 + h
    ch = base + 112
    cards = []
    for r, label, effects, _, glyph in RENDERERS:
        if r not in rs:
            continue
        card = rs[r]["exr"][:-4] + "_card.png"
        line1, line2 = info(rs[r], r, layer)
        colour = hexrgb(COLOURS["dark"][r])
        oiio(rs[r]["exr"], "--ch", "R,G,B", "--croptofull", "--resize", "%dx%d" % (w, h), "--clamp:min=0",
             "--colorconvert", "linear", "sRGB", "--clamp:min=0:max=1",
             "--create", "%dx%d" % (cw, ch), "3", "--fill:color=" + CARD["bg"], "%dx%d+0+0" % (cw, ch),
             "--fill:color=" + colour, "%dx4+0+0" % cw, "--paste", "+8+12",
             "--text:x=10:y=%d:size=22:color=%s:font=seguisym" % (base + 32, colour), GLYPH[glyph],
             "--text:x=36:y=%d:size=22:color=%s:font=seguisb" % (base + 32, CARD["text"]), label,
             "--text:x=10:y=%d:size=17:color=%s:font=segoeui" % (base + 58, CARD["text"]), effects,
             "--text:x=10:y=%d:size=17:color=%s:font=segoeui" % (base + 82, CARD["dim"]), line1,
             "--text:x=10:y=%d:size=17:color=%s:font=segoeui" % (base + 104, CARD["dim"]), line2,
             "-o", card)
        cards.append(card)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    oiio(*cards, "--mosaic:pad=12", "%dx%d" % (min(2, len(cards)), (len(cards) + 1) // 2), "-d", "uint8",
         "--attrib", "Compression", "jpeg:82", "-o", path)


def section(results=None):
    """The Production scenes section of docs/gallery.md, as lines; [] before the first render."""
    if results is None:
        results = json.load(open(RESULTS)) if os.path.exists(RESULTS) else {}
    if not results:
        return []
    md = [HEADING, "",
          "Intel's Sponza with its curtains, ivy, cypress, candles, knight, flood and explosion add-ons "
          "and a 9,000-agent crowd, rendered with husk at 960 × 540 (the crowd at 1280 × 720): "
          "● **Eldr raster + SSGI**, every effect without ray tracing (`tests/scenes/cornell/eldr_default.usda`); "
          "◆ **Eldr ray traced** (`tests/scenes/cornell/eldr_final.usda`); ▲ **Karma XPU** (256 path-traced "
          "samples) and ■ **Karma CPU** (128), both with 4 diffuse bounces and OIDN, stopped after 10 minutes "
          "(*time-limited* on the card). Karma CPU is the brightness "
          "reference. *Render* and *load* come from each renderer's own log, as in the Methodology above: "
          "Eldr's render passes less the loading in them, timed on a second run with a warm shader cache; "
          "Karma's wall clock less load and update. *GPU* is the memory Eldr's render took. "
          "The layers are in `tests/scenes/production`; `python tests/render_production.py` renders them again.", "",
          "Hardware: " + hardware(), ""]
    for view, heading, what in VIEWS:
        r = results.get(view)
        if not r:
            continue
        alt = "; ".join("%s %s render" % (LABEL[k], duration(r[k]["sec"])) for k in LABEL if k in r)
        md += ["### " + heading, "", what, "", "![%s: %s](images/gallery/production/%s.jpg)" % (heading, alt, view), ""]
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
        if not rs:
            continue
        ref = rs.get("karma_cpu", {}).get("exr")
        for k, x in rs.items():
            st = stats(x["exr"], ref) if ref and k != "karma_cpu" else None
            x["ratio"] = st[1] if st else None
        panel(view, rs, os.path.join(IMAGES, view + ".jpg"))
        results[view] = {k: {f: v for f, v in x.items() if f != "exr"} for k, x in rs.items()}
        json.dump(results, open(RESULTS, "w"), indent=1)
    splice(section(results))


if __name__ == "__main__":
    main()
