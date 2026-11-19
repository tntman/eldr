"""Render the gallery with Eldr and Karma and write docs/gallery.md, docs/images/gallery/ and README.md's
Gallery section.

  hython tests/render_gallery.py [outdir] [--only base,glossy] [--force] [--build <repo>/build-h22l]

Two sections. The Cornell box (tests/scenes/cornell: cornell.usda and every variants/*.usda, 512 x 512)
and the feature scenes (FEATURES, folders of showcase/). Each scene renders four ways through husk:
  eldr_default  Eldr's effects without ray tracing: tests/scenes/cornell/eldr_default.usda over the
                scene's settings (/Render/Settings, or a showcase's /Render/eldr_default)
  eldr_final    ray traced: eldr_final.usda over /Render/Settings, or a showcase's /Render/eldr_final
  karma_xpu     BRAY_HdKarmaXPU with the scene's Karma settings (/Render/Settings or /Render/karma)
  karma_cpu     BRAY_HdKarma, the same: the brightness reference
Renders land in outdir (default %TEMP%/eldr_gallery) and are reused unless --force. A renderer that
writes no image (Karma XPU on a GPU it can't use) leaves its card out.
Times: Eldr's time in its render pass (HDVK_TIMING=2, the sum of execute_ms), Karma's render time from
its log (husk -V2: total less load and update), else the husk wall clock. The Eldr default log must not show a ray tracing pipeline
compiling ("HdVk rt pipeline"); the script warns when it does.
Each scene's picture is one JPEG: a row of cards (render, renderer, resolution, samples, time,
brightness against Karma CPU) drawn with Houdini's hoiiotool. Brightness is the mean luminance ratio
from tests/compare_renderers.py.
"""
import argparse
import csv as csvfile
import glob
import json
import math
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from compare_renderers import HUSK, karma_layer, oiio, run, stats  # noqa: E402

# hython sets OCIO to Houdini's config, where hoiiotool's "linear" isn't the sRGB primaries' linear.
os.environ.pop("OCIO", None)
CORNELL = os.path.join(HERE, "scenes", "cornell")
SHOWCASE = os.path.join(REPO, "showcase")
IMAGES = os.path.join(REPO, "docs", "images", "gallery")
FILM = 256  # eldr:filmSamples of eldr_default.usda, eldr_final.usda and the showcases' eldr_final prims

RENDERERS = [("eldr_default", "Eldr default", "circle"), ("eldr_final", "Eldr final", "diamond"),
             ("karma_xpu", "Karma XPU", "triangle"), ("karma_cpu", "Karma CPU", "square")]
LABEL = {r: l for r, l, _ in RENDERERS}
GLYPH = {"circle": "●", "diamond": "◆", "triangle": "▲", "square": "■"}
COLOURS = {"light": {"eldr_default": "#2f6fd0", "eldr_final": "#d9732b", "karma_xpu": "#8a4fd0", "karma_cpu": "#6e7781",
                     "text": "#1f2328", "grid": "#d0d7de", "bg": "#ffffff"},
           "dark": {"eldr_default": "#6ea8fe", "eldr_final": "#f0a35e", "karma_xpu": "#c29bff", "karma_cpu": "#a0a8b0",
                    "text": "#e6edf3", "grid": "#30363d", "bg": "#0d1117"}}
CARD = {"bg": "0.086,0.106,0.133", "text": "0.90,0.93,0.95", "dim": "0.62,0.66,0.70"}  # #161b22 card

# Cornell variants: heading, and a description where the layer's doc string (written for hdUE) doesn't fit.
TITLES = {"base": "Global illumination", "glossy": "Glossy materials", "depth_of_field": "Depth of field",
          "motion_blur": "Motion blur", "subdivision": "Subdivision surfaces", "instances": "Instances, points, curves",
          "volume": "Volumes (OpenVDB)", "ies": "IES profile", "light_linking": "Light linking",
          "clearcoat": "Clearcoat", "render_region": "Render region", "lens_shift": "Lens shift",
          "orthographic": "Orthographic camera", "exposure": "Camera exposure",
          "post_processing": "Unreal post-processing settings", "displacement": "Displacement",
          "height_fog": "Unreal height fog settings", "uv_sets": "UV sets", "glass": "Glass and caustics",
          "subsurface": "Subsurface scattering", "light_texture": "Textured area light", "hair": "Hair",
          "lobes": "Sheen, thin film, anisotropy", "emissive": "Emissive surface lighting", "holdout": "Holdout",
          "light_groups": "Light groups"}
DESCRIPTIONS = {
    "base": "The CC0 Cornell box with its measured camera and a RectLight matching the ceiling light.",
    "displacement": "Preview Surface displacement on a single quad: blurred checker bumps, 0.6 m high.",
    "height_fog": "Unreal's height fog settings (`unreal:`) on the base scene; Eldr and Karma ignore them.",
    "light_linking": "A warm light linked only to the tall box and a cool one only to the short box; the ceiling "
                     "light lights everything.",
    "post_processing": "Unreal's post-processing settings (`unreal:`) on the base scene; Eldr and Karma ignore them "
                       "and render the linear image.",
    "subdivision": "A Catmull-Clark cube (USD's default scheme) with a red GeomSubset on two faces.",
    "render_region": "A render region: dataWindowNDC (0.25, 0.15)-(0.75, 0.65) renders only that part of the frame.",
}
# What a card shows in Eldr that differs from Karma on purpose or by a known limitation.
NOTES = {}
# Feature scenes: (name, showcase folder, heading). Their eldr_default prim gets eldr_default.usda.
FEATURES = [("material_lobes", "2026-10-30_22_material_lobes", "Material lobes"),
            ("subsurface", "2026-10-30_13_subsurface", "Skin, jade and leaves"),
            ("caustics", "2026-10-29_13_caustics", "Caustics"),
            ("reflections", "2026-11-04_01_reflections", "Reflections"),
            ("curves_points", "2026-10-30_19_curves_points", "Curves, points and hair"),
            ("light_shapes", "2026-10-30_07_light_shapes", "Light shapes"),
            ("many_lights", "2026-10-30_06_many_lights", "Many lights"),
            ("volume_scattering", "2026-10-29_26_volume_multiple_scattering", "Volume multiple scattering"),
            ("displacement", "2026-10-30_18_displacement", "Displacement shaders")]


def fwd(p):
    return p.replace("\\", "/")


def doc(path):
    m = re.search(r'doc = (?:"""(.*?)"""|"(.*?)")', open(path, encoding="utf-8").read(), re.S)
    return " ".join((m.group(1) or m.group(2)).split()) if m else ""


def find(pattern, *texts, default=None):
    for t in texts:
        m = re.search(pattern, t)
        if m:
            return m.groups()
    return default


def prim_block(text, name):
    """The text of `def RenderSettings "<name>"` up to the next settings prim."""
    i = text.find('RenderSettings "%s"' % name)
    if i < 0:
        return ""
    j = text.find("RenderSettings", i + 15)
    return text[i:j if j > 0 else len(text)]


def header(usd):
    """upAxis and metersPerUnit of a scene's root layer, for a wrapper layer over it."""
    t = open(usd, encoding="utf-8").read()[:2000]
    return "".join("    %s\n" % m.group(0) for m in re.finditer(r'(upAxis = "\w+"|metersPerUnit = [\d.]+)', t))


def layer(path, usd, sublayer=None, reference=None):
    """A wrapper layer: `sublayer` over `usd`, or `usd` with <reference> added to one settings prim."""
    subs = [fwd(sublayer)] if sublayer else []
    with open(path, "w", newline="\n") as f:
        f.write('#usda 1.0\n(\n%s    subLayers = [%s]\n)\n' % (
            header(usd), ", ".join("@%s@" % s for s in subs + [fwd(usd)])))
        if reference:
            prim, src = reference
            f.write('\nover "Render"\n{\n    over "%s" (\n        prepend references = @%s@</Render/Settings>\n'
                    '    )\n    {\n    }\n}\n' % (prim, fwd(src)))
    return path


def scenes():
    """[{name, section, title, desc, usd, jobs: {renderer: (plugin, usd, settings prim)}, res, spp}]."""
    out = []
    base = open(os.path.join(CORNELL, "cornell.usda"), encoding="utf-8").read()
    paths = [("base", os.path.join(CORNELL, "cornell.usda"))]
    paths += [(os.path.basename(p)[:-5], p) for p in sorted(glob.glob(os.path.join(CORNELL, "variants", "*.usda")))]
    for name, usd in paths:
        t = open(usd, encoding="utf-8").read()
        out.append(dict(name=name, section="cornell", title=TITLES.get(name, name), usd=usd,
                        desc=DESCRIPTIONS.get(name) or doc(usd),
                        res=tuple(map(int, find(r"resolution = \((\d+), (\d+)\)", t, base))),
                        spp=int(find(r"pathtracedsamples = (\d+)", t, base)[0]),
                        prims=dict(eldr_default="/Render/Settings", eldr_final="/Render/Settings",
                                   karma="/Render/Settings")))
    for name, folder, title in FEATURES:
        usd = os.path.join(SHOWCASE, folder, "scene.usda")
        karma = prim_block(open(usd, encoding="utf-8").read(), "karma")
        summary = open(os.path.join(SHOWCASE, folder, "README.md"), encoding="utf-8").read().split("<!-- summary -->")
        out.append(dict(name=name, section="features", title=title, usd=usd, folder=folder,
                        desc=" ".join(summary[1].split()) if len(summary) > 2 else "",
                        res=tuple(map(int, find(r"resolution = \((\d+), (\d+)\)", karma, default=(960, 540)))),
                        spp=int(find(r"pathtracedsamples = (\d+)", karma, default=("256",))[0]),
                        prims=dict(eldr_default="/Render/eldr_default", eldr_final="/Render/eldr_final",
                                   karma="/Render/karma")))
    return out


def karma_seconds(log):
    """Karma's render time from a husk -V2 log: the frame's total wall clock less its load and update."""
    t = open(log, errors="replace").read() if os.path.exists(log) else ""

    def last(what):
        m = re.findall(r"%s Wall Clock Time: (\d+):(\d+):(\d+(?:\.\d+)?)" % what, t)
        return int(m[-1][0]) * 3600 + int(m[-1][1]) * 60 + float(m[-1][2]) if m else None
    total, load, update = last("Total"), last("Load"), last("Update")
    return total - (load or 0) - (update or 0) if total is not None else None


def render(s, outdir, build, force, new):
    """{renderer: {exr, sec, wall, rt}} for one scene; a renderer without an image is left out. Renders
    only when `new` (else uses what earlier runs left)."""
    d = os.path.join(outdir, s["section"], s["name"])
    os.makedirs(d, exist_ok=True)
    usd, prims = s["usd"], s["prims"]
    if s["section"] == "cornell":
        default = layer(os.path.join(d, "default_layer.usda"), usd, sublayer=os.path.join(CORNELL, "eldr_default.usda"))
        final = layer(os.path.join(d, "final_layer.usda"), usd, sublayer=os.path.join(CORNELL, "eldr_final.usda"))
    karma = usd
    if s["section"] != "cornell":
        default = layer(os.path.join(d, "default_layer.usda"), usd,
                        reference=("eldr_default", os.path.join(CORNELL, "eldr_default.usda")))
        final = usd
        # Karma: domes shown to the camera as Eldr shows them, beauty only (a showcase's products carry
        # Eldr's vars), Path Traced at the karma prim's samples (Karma's default Variance mode stops early).
        from pxr import Usd
        karma = karma_layer(Usd.Stage.Open(usd), usd, os.path.join(d, "karma_layer.usda"), True, prims["karma"],
                            s["spp"])
    env = dict(os.environ, MSYS_NO_PATHCONV="1", HOUDINI_PACKAGE_DIR=os.path.join(build, "houdini"),
               PXR_PLUGINPATH_NAME=build, HDVK_TIMING="2")
    env.setdefault("HOUDINI_USER_PREF_DIR", os.path.join(outdir, "hprefs__HVER__"))
    jobs = {"eldr_default": ("HdVkRendererPlugin", default, prims["eldr_default"]),
            "eldr_final": ("HdVkRendererPlugin", final, prims["eldr_final"]),
            "karma_xpu": ("BRAY_HdKarmaXPU", karma, prims["karma"]),
            "karma_cpu": ("BRAY_HdKarma", karma, prims["karma"])}
    times = os.path.join(d, "times.json")
    known = json.load(open(times)) if os.path.exists(times) else {}
    out = {}
    for r, (plugin, scene, prim) in jobs.items():
        exr, log, csv = (os.path.join(d, r + ext) for ext in (".exr", ".log", ".csv"))
        if new and (force or r not in known):
            for f in (exr, csv):
                if os.path.exists(f):
                    os.remove(f)
            code, wall = run([HUSK, "-R", plugin, "-s", prim, "-f", "1", "-n", "1", "-V", "2", "-o", exr, scene],
                             log, dict(env, HDVK_TIMING_CSV=csv))
            rows = list(csvfile.DictReader(open(csv))) if os.path.exists(csv) else []
            sec = sum(float(x["execute_ms"]) for x in rows) / 1000 if rows else karma_seconds(log)
            rt = "HdVk rt pipeline" in open(log, errors="replace").read()
            known[r] = dict(ok=os.path.exists(exr), wall=wall, sec=sec if sec is not None else wall, rt=rt)
            print("%-9s %-18s %-13s %s %.1f s (wall %.1f s)%s" % (
                s["section"], s["name"], r, "ok" if known[r]["ok"] else "FAILED (%d)" % code, known[r]["sec"], wall,
                "  WARNING: ray tracing started" if rt and r == "eldr_default" else ""), flush=True)
            json.dump(known, open(times, "w"))
        if known.get(r, {}).get("ok"):
            out[r] = dict(known[r], exr=exr)
    return out


def duration(sec):
    return "%.0f ms" % (sec * 1000) if sec < 1 else "%.1f s" % sec


def sampling(r, spp):
    return "%d film samples" % FILM if r.startswith("eldr") else "%d spp" % spp


def brightness(row, r):
    if r == "karma_cpu":
        return "Brightness reference"
    v = row.get(r + "_ratio")
    return "Brightness %.2f× Karma CPU" % v if v is not None else "Brightness -"


def hexrgb(h):
    return ",".join("%.3f" % (int(h[i:i + 2], 16) / 255) for i in (1, 3, 5))


def panel(row, rs, path):
    """The scene's cards side by side as one JPEG."""
    w, h = 512, round(512 * row["res"][1] / row["res"][0])
    cw, base = w + 16, 12 + h
    ch = base + 84
    cards = []
    for r, label, marker in RENDERERS:
        if r not in rs:
            continue
        card = rs[r]["exr"][:-4] + "_card.png"
        line1 = "%d × %d · %s · %s" % (*row["res"], sampling(r, row["spp"]), duration(rs[r]["sec"]))
        oiio(rs[r]["exr"], "--ch", "R,G,B", "--croptofull", "--resize", "%dx%d" % (w, h), "--clamp:min=0",
             "--colorconvert", "linear", "sRGB", "--clamp:min=0:max=1",
             "--create", "%dx%d" % (cw, ch), "3", "--fill:color=" + CARD["bg"], "%dx%d+0+0" % (cw, ch),
             "--fill:color=" + hexrgb(COLOURS["dark"][r]), "%dx4+0+0" % cw, "--paste", "+8+12",
             "--text:x=10:y=%d:size=18:color=%s:font=seguisym" % (base + 30, hexrgb(COLOURS["dark"][r])), GLYPH[marker],
             "--text:x=32:y=%d:size=18:color=%s:font=seguisb" % (base + 30, CARD["text"]), label,
             "--text:x=10:y=%d:size=15:color=%s:font=segoeui" % (base + 54, CARD["dim"]), line1,
             "--text:x=10:y=%d:size=15:color=%s:font=segoeui" % (base + 75, CARD["dim"]), brightness(row, r),
             "-o", card)
        cards.append(card)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    oiio(*cards, "--mosaic:pad=12", "%dx1" % len(cards), "-d", "uint8", "--attrib", "Compression", "jpeg:85",
         "-o", path)


def marker(kind, x, y, colour, size=5):
    if kind == "circle":
        return '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/>' % (x, y, size, colour)
    if kind == "square":
        return '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s"/>' % (
            x - size * 0.9, y - size * 0.9, size * 1.8, size * 1.8, colour)
    pts = {"diamond": [(0, -1.25), (1.1, 0), (0, 1.25), (-1.1, 0)], "triangle": [(0, -1.2), (1.1, 0.85), (-1.1, 0.85)]}
    return '<polygon points="%s" fill="%s"/>' % (
        " ".join("%.1f,%.1f" % (x + a * size, y + b * size) for a, b in pts[kind]), colour)


def dots(rows, keys, title, theme, scale, ticks, fmt, ref=None):
    """A dot plot: one row per scene, one marker per renderer on a shared axis; scale(v) is 0..1."""
    c = COLOURS[theme]
    left, width, step, top = 220, 520, 18, 62
    height = top + len(rows) * step + 36
    s = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
         'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif" font-size="12">'
         % (left + width + 30, height, left + width + 30, height),
         '<rect width="100%%" height="100%%" fill="%s"/>' % c["bg"],
         '<text x="10" y="20" font-size="15" font-weight="600" fill="%s">%s</text>' % (c["text"], title)]
    x = 10
    for k in keys:
        kind = next(m for r, _, m in RENDERERS if r == k)
        s.append(marker(kind, x + 5, 38, c[k]) + '<text x="%d" y="42" fill="%s">%s</text>' % (x + 14, c["text"], LABEL[k]))
        x += 14 + 7 * len(LABEL[k]) + 22
    for v in ticks:
        gx = left + width * scale(v)
        s.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"%s/>' % (
            gx, top - 8, gx, height - 26, c["grid"], ' stroke-width="2"' if v == ref else ""))
        s.append('<text x="%.1f" y="%d" text-anchor="middle" fill="%s">%s</text>' % (gx, height - 10, c["text"], fmt(v)))
    for i, r in enumerate(rows):
        y = top + i * step + step / 2
        if i % 2:
            s.append('<rect x="0" y="%.1f" width="%d" height="%d" fill="%s" opacity="0.35"/>' % (
                y - step / 2, left + width + 30, step, c["grid"]))
        s.append('<text x="%d" y="%.1f" text-anchor="end" dominant-baseline="middle" fill="%s">%s</text>'
                 % (left - 10, y, c["text"], r["title"]))
        for k in keys:
            v = r.get(k)
            if v is not None:
                kind = next(m for rr, _, m in RENDERERS if rr == k)
                s.append('<g><title>%s, %s: %s</title>%s</g>' % (
                    r["title"], LABEL[k], fmt(v), marker(kind, left + width * min(1, max(0, scale(v))), y, c[k], 4.5)))
    return "\n".join(s + ["</svg>\n"])


def charts(rows):
    keys = [r for r, _, _ in RENDERERS]
    times = [v for row in rows for k in keys for v in [row.get(k + "_sec")] if v]
    lo = 10 ** math.floor(math.log10(min(times))) if times else 0.01
    hi = 10 ** math.ceil(math.log10(max(times))) if times else 100
    ticks = [10 ** e for e in range(round(math.log10(lo)), round(math.log10(hi)) + 1)]
    trows = [dict(title=r["title"], **{k: r.get(k + "_sec") for k in keys}) for r in rows]
    ratios = [v for row in rows for k in keys[:3] for v in [row.get(k + "_ratio")] if v]
    bmax = max(1.5, math.ceil(max(ratios + [1]) * 4) / 4)
    btick = [i * 0.25 for i in range(int(bmax / 0.25) + 1)]
    brows = [dict(title=r["title"], **{k: r.get(k + "_ratio") for k in keys[:3]}) for r in rows]
    for theme in COLOURS:
        with open(os.path.join(IMAGES, "times_%s.svg" % theme), "w", newline="\n") as f:
            f.write(dots(trows, keys, "Render time per scene (log scale)", theme,
                         lambda v: (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)), ticks,
                         lambda v: duration(v)))
        with open(os.path.join(IMAGES, "brightness_%s.svg" % theme), "w", newline="\n") as f:
            f.write(dots(brows, keys[:3], "Mean brightness relative to Karma CPU", theme, lambda v: v / bmax, btick,
                         lambda v: "%.2f×" % v, ref=1.0))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("outdir", nargs="?", default=os.path.join(tempfile.gettempdir(), "eldr_gallery"))
    p.add_argument("--only")
    p.add_argument("--force", action="store_true")
    p.add_argument("--build", default=os.path.join(REPO, "build-h22l"))
    a = p.parse_args()
    os.makedirs(IMAGES, exist_ok=True)
    rows = []
    for s in scenes():
        new = not a.only or s["name"] in a.only.split(",")
        rs = render(s, a.outdir, a.build, a.force, new)
        if not rs:
            continue
        row = dict(s, image="%s%s.jpg" % ("features/" if s["section"] == "features" else "", s["name"]),
                   renderers=list(rs))
        ref = rs.get("karma_cpu", {}).get("exr")
        for r, v in rs.items():
            row[r + "_sec"] = v["sec"]
            st = stats(v["exr"], ref) if r != "karma_cpu" else None
            row[r + "_ratio"] = st[1] if st else None
            row[r + "_rt"] = v["rt"]
        if new:
            panel(row, rs, os.path.join(IMAGES, row["image"]))
        rows.append(row)
    charts(rows)
    write_docs(rows)
    for row in rows:
        if row.get("eldr_default_rt"):
            print("WARNING: Eldr default started ray tracing in", row["name"])
        missing = [LABEL[r] for r, _, _ in RENDERERS if r not in row["renderers"]]
        if missing:
            print("no image:", row["name"], ", ".join(missing))


def table(rows, link):
    keys = [r for r, _, _ in RENDERERS]
    out = ["| Scene | %s | Brightness vs Karma CPU (default / final / XPU) |" % " | ".join(LABEL[k] for k in keys),
           "|---|" + "---:|" * len(keys) + "---|"]
    for r in rows:
        t = [duration(r[k + "_sec"]) if r.get(k + "_sec") is not None else "-" for k in keys]
        b = " / ".join("%.2f×" % r[k + "_ratio"] if r.get(k + "_ratio") else "-" for k in keys[:3])
        out.append("| %s | %s | %s |" % (link(r), " | ".join(t), b))
    return "\n".join(out)


def slug(title):
    return re.sub(r"[^\w\- ]", "", title.lower()).replace(" ", "-")


INTRO = ("Every scene renders four ways with husk: "
         "● **Eldr default**, every effect that works without ray tracing (screen-space GI, reflections and "
         "refraction, planar mirrors, ambient occlusion, subsurface, soft shadow maps, denoising, 256 film samples: "
         "`tests/scenes/cornell/eldr_default.usda`); "
         "◆ **Eldr final**, ray traced (reflections, refraction, shadows, GI, final gather, caustics, denoising, "
         "256 film samples: `tests/scenes/cornell/eldr_final.usda`); "
         "▲ **Karma XPU** and ■ **Karma CPU** with the scene's Karma settings. Karma CPU is the brightness "
         "reference.")

METHODOLOGY = ["### Methodology", "",
               "| | |", "|---|---|",
               "| Machine | Windows, NVIDIA GTX 1080 Ti; Karma CPU on the same machine |",
               "| Eldr default | `tests/scenes/cornell/eldr_default.usda` over the scene's settings (a showcase's "
               "`eldr_default` prim), `husk -R HdVkRendererPlugin`; no ray tracing starts |",
               "| Eldr final | `tests/scenes/cornell/eldr_final.usda` over `/Render/Settings`, or a showcase's "
               "`eldr_final` prim |",
               "| Karma | `BRAY_HdKarmaXPU` and `BRAY_HdKarma` with the scene's settings (Cornell box: 512 path-traced "
               "samples, 8 diffuse bounces; showcases: their `karma` prim, Path Traced, beauty only, domes visible to the camera as in Eldr) |",
               "| Time | Eldr: time in its render pass (`HDVK_TIMING=2`). Karma: its render time from the husk log. "
               "Scene load and startup are not counted |",
               "| Brightness | Mean luminance against Karma CPU's (1.00×: as bright as Karma CPU) |",
               "| Regenerate | `hython tests/render_gallery.py [outdir] [--only base,glossy] [--force]` "
               "(renders, pictures, this page and README.md's gallery) |", ""]


def sections(rows, img, hero=None):
    """The two scene sections: a heading, a description and the cards' picture per scene."""
    md = []
    for sec, head, intro in [
            ("cornell", "Cornell box: one feature per scene",
             "The CC0 Cornell box (`tests/scenes/cornell`) plus one small variant layer, rendered at 512 × 512."),
            ("features", "Feature scenes",
             "Showcase scenes (`showcase/`) for major features, rendered at their own resolution.")]:
        md += ["## " + head, "", intro, ""]
        if sec == "features" and hero:
            md += hero
        for r in rows:
            if r["section"] != sec:
                continue
            md += ["### " + r["title"], "", r["desc"] + (" (`showcase/%s`)" % r["folder"] if sec == "features" else ""), ""]
            if r["name"] in NOTES and sec == "cornell":
                md += ["*In Eldr:* " + NOTES[r["name"]], ""]
            md += ["![%s: %s](%s%s)" % (r["title"], ", ".join(LABEL[k] for k in r["renderers"]), img, r["image"]), ""]
    return md


def picture(stem, alt, prefix):
    return ('<picture>\n  <source media="(prefers-color-scheme: dark)" srcset="%s%s_dark.svg">\n'
            '  <img alt="%s" src="%s%s_light.svg" width="100%%">\n</picture>' % (prefix, stem, alt, prefix, stem))


def write_docs(rows):
    md = ["# Gallery", "", INTRO, "",
          "![Render time per scene](images/gallery/times_light.svg)", "",
          "![Brightness relative to Karma CPU per scene](images/gallery/brightness_light.svg)", "",
          table(rows, lambda r: "[%s](#%s)" % (r["title"], slug(r["title"]))), ""] + METHODOLOGY
    md += sections(rows, "images/gallery/")
    from render_production import section  # the Production scenes section, last rendered by render_production.py
    md += section()
    with open(os.path.join(REPO, "docs", "gallery.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(md))
    hero = ["![Material lobes: clearcoat, sheen, fuzz, thin film, anisotropic highlights, hair and toon shading]"
            "(docs/images/hero.jpg)", "",
            "*Clearcoat, sheen, fuzz, thin film, anisotropic highlights, hair and toon shading, rendered by Eldr "
            "(`showcase/2026-10-30_22_material_lobes`).*", ""]
    readme = os.path.join(REPO, "README.md")
    text = open(readme, encoding="utf-8").read()
    gallery = ["## Gallery", "", INTRO, "",
               picture("times", "Render time per scene for each renderer", "docs/images/gallery/"), "",
               picture("brightness", "Mean brightness relative to Karma CPU per scene", "docs/images/gallery/"), "",
               table(rows, lambda r: "[%s](docs/gallery.md#%s)" % (r["title"], slug(r["title"]))), "",
               "[docs/gallery.md](docs/gallery.md) has the methodology.", ""]
    gallery += ["#" + line if line.startswith("##") else line for line in sections(rows, "docs/images/gallery/", hero)]
    start = text.index("## Gallery")
    end = text.index("A few features, off and on:")
    with open(readme, "w", encoding="utf-8", newline="\n") as f:
        f.write(text[:start] + "\n".join(gallery) + "\n" + text[end:])


if __name__ == "__main__":
    main()
