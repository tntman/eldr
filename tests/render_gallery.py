"""Render the Cornell box gallery (tests/scenes/cornell) with Eldr and Karma and write docs/gallery.md,
docs/images/gallery/ and README.md's Gallery section.

  python tests/render_gallery.py [outdir] [--only base,glossy] [--force] [--build <repo>/build-h22l]

Each scene (cornell.usda and every variants/*.usda) renders through husk at the scene's 512 x 512:
  eldr_default  the scene's /Render/Settings as authored, -R HdVkRendererPlugin
  eldr_final    the same with tests/scenes/cornell/eldr_final.usda sublayered over it
  karma         BRAY_HdKarma with the scene's settings (512 path-traced samples, 8 diffuse bounces)
Renders land in outdir (default %TEMP%/eldr_gallery) and are reused unless --force. Times are wall
clock per husk run (startup and scene load included); HDVK_TIMING=2 adds Eldr's time in its render pass
(sum of execute_ms) and the GPU time of one film sample (the last gpu_ms).
Statistics against Karma come from tests/compare_renderers.py.
"""
import argparse
import csv as csvfile
import glob
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from compare_renderers import HUSK, oiio, run, stats  # noqa: E402

CORNELL = os.path.join(HERE, "scenes", "cornell")
IMAGES = os.path.join(REPO, "docs", "images", "gallery")
# What a variant shows in Eldr where its doc string (written for hdUE) doesn't say.
NOTES = {
    "base": "the default settings have no bounced light (GI is off unless asked for), so the ceiling and shadowed "
            "sides are black; the final settings add GI and final gather.",
    "height_fog": "Eldr and Karma ignore the `unreal:` fog settings: the image is the base scene.",
    "post_processing": "Eldr and Karma ignore the `unreal:` post-processing settings and render the linear image.",
    "subdivision": "Eldr refines the Catmull-Clark cube with OpenSubdiv; `unreal:subdivisionLevel` is hdUE's own.",
}
RENDERERS = [("eldr_default", "Eldr default"), ("eldr_final", "Eldr final"), ("karma", "Karma")]
COLOURS = {"light": {"eldr_default": "#3b7dd8", "eldr_final": "#d9822b", "karma": "#7a7a7a",
                     "text": "#1f2328", "grid": "#d0d7de", "bg": "#ffffff"},
           "dark": {"eldr_default": "#6ea8fe", "eldr_final": "#f0a35e", "karma": "#a0a0a0",
                    "text": "#e6edf3", "grid": "#30363d", "bg": "#0d1117"}}


def scenes():
    out = [("base", os.path.join(CORNELL, "cornell.usda"))]
    out += [(os.path.basename(p)[:-5], p) for p in sorted(glob.glob(os.path.join(CORNELL, "variants", "*.usda")))]
    return out


def doc(path):
    m = re.search(r'doc = (?:"""(.*?)"""|"(.*?)")', open(path, encoding="utf-8").read(), re.S)
    return " ".join((m.group(1) or m.group(2)).split()) if m else ""


def render(name, usd, outdir, build, force):
    """{renderer: (exr, wall s, Eldr execute s, GPU ms per sample)} for one scene."""
    d = os.path.join(outdir, name)
    os.makedirs(d, exist_ok=True)
    final = os.path.join(d, "final_layer.usda")
    with open(final, "w") as f:
        f.write('#usda 1.0\n(\n    subLayers = [@%s@, @%s@]\n)\n' % (
            os.path.join(CORNELL, "eldr_final.usda").replace("\\", "/"), usd.replace("\\", "/")))
    env = dict(os.environ, MSYS_NO_PATHCONV="1", HOUDINI_PACKAGE_DIR=os.path.join(build, "houdini"),
               PXR_PLUGINPATH_NAME=build, HDVK_TIMING="2")
    env.setdefault("HOUDINI_USER_PREF_DIR", os.path.join(outdir, "hprefs__HVER__"))
    jobs = {"eldr_default": ("HdVkRendererPlugin", usd), "eldr_final": ("HdVkRendererPlugin", final),
            "karma": ("BRAY_HdKarma", usd)}
    times = os.path.join(d, "times.json")
    known = json.load(open(times)) if os.path.exists(times) else {}
    out = {}
    for r, (plugin, scene) in jobs.items():
        exr, log, csv = (os.path.join(d, r + ext) for ext in (".exr", ".log", ".csv"))
        if force or not os.path.exists(exr) or r not in known:
            if os.path.exists(csv):
                os.remove(csv)
            code, sec = run([HUSK, "-R", plugin, "-s", "/Render/Settings", "-f", "1", "-n", "1", "-o", exr, scene],
                            log, dict(env, HDVK_TIMING_CSV=csv))
            # HDVK_TIMING=2: a row per render pass execute; execute_ms is the time in Eldr (sync excluded),
            # gpu_ms the GPU time of the execute's last sample.
            rows = list(csvfile.DictReader(open(csv))) if os.path.exists(csv) else []
            known[r] = (sec, sum(float(x["execute_ms"]) for x in rows) / 1000 if rows else None,
                        float(rows[-1]["gpu_ms"]) if rows else None)
            print("%-16s %-13s %s %.1f s" % (name, r, "ok" if os.path.exists(exr) else "FAILED (%d)" % code, sec),
                  flush=True)
            json.dump(known, open(times, "w"))
        out[r] = (exr, *known[r])
    return out


def panel(name, renders):
    """docs/images/gallery/<name>.jpg: the three renders side by side, sRGB, labelled."""
    args = []
    for r, label in RENDERERS:
        exr, sec, _, _ = renders[r]
        args += [exr, "--ch", "R,G,B", "--croptofull", "--resize", "512x512", "--clamp:min=0",
                 "--colorconvert", "linear", "sRGB",
                 "--text:x=10:y=26:size=20:color=1,1,0:shadow=2", "%s  %.1f s" % (label, sec)]
    oiio(*args, "--mosaic", "3x1", "--clamp:min=0:max=1", "-d", "uint8", "--attrib", "Compression", "jpeg:82",
         "-o", os.path.join(IMAGES, name + ".jpg"))


def bars(rows, keys, title, unit, fmt, theme):
    """A horizontal grouped bar chart, one group per scene, as SVG text."""
    c = COLOURS[theme]
    left, width, bar, gap, top = 130, 560, 9, 8, 56
    vmax = max(r[k] for r in rows for k in keys if r[k] is not None) or 1
    height = top + len(rows) * (len(keys) * bar + gap) + 30
    s = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
         'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif" font-size="12">'
         % (left + width + 90, height, left + width + 90, height),
         '<rect width="100%%" height="100%%" fill="%s"/>' % c["bg"],
         '<text x="10" y="20" font-size="15" font-weight="600" fill="%s">%s</text>' % (c["text"], title)]
    x = left
    for k in keys:
        label = dict(RENDERERS)[k]
        s.append('<rect x="%d" y="32" width="10" height="10" fill="%s"/><text x="%d" y="41" fill="%s">%s</text>'
                 % (x, c[k], x + 14, c["text"], label))
        x += 14 + 8 * len(label) + 16
    for i in range(5):
        gx = left + width * i / 4
        s.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>' % (gx, top - 4, gx, height - 26, c["grid"]))
        s.append('<text x="%.1f" y="%d" text-anchor="middle" fill="%s">%s%s</text>'
                 % (gx, height - 10, c["text"], fmt % (vmax * i / 4), unit))
    y = top
    for r in rows:
        gh = len(keys) * bar
        s.append('<text x="%d" y="%.1f" text-anchor="end" dominant-baseline="middle" fill="%s">%s</text>'
                 % (left - 8, y + gh / 2, c["text"], r["name"]))
        for k in keys:
            v = r[k]
            if v is not None:
                s.append('<rect x="%d" y="%d" width="%.1f" height="%d" fill="%s"/>'
                         % (left, y, max(1, width * v / vmax), bar - 1, c[k]))
                s.append('<text x="%.1f" y="%d" font-size="10" dominant-baseline="middle" fill="%s">%s</text>'
                         % (left + width * v / vmax + 4, y + bar / 2, c["text"], fmt % v + unit))
            y += bar
        y += gap
    return "\n".join(s + ["</svg>\n"])


def picture(stem, alt):
    return ('<picture>\n  <source media="(prefers-color-scheme: dark)" srcset="docs/images/gallery/%s_dark.svg">\n'
            '  <img alt="%s" src="docs/images/gallery/%s_light.svg" width="100%%">\n</picture>' % (stem, alt, stem))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("outdir", nargs="?", default=os.path.join(tempfile.gettempdir(), "eldr_gallery"))
    p.add_argument("--only")
    p.add_argument("--force", action="store_true")
    p.add_argument("--build", default=os.path.join(REPO, "build-h22l"))
    a = p.parse_args()
    os.makedirs(IMAGES, exist_ok=True)
    rows = []
    for name, usd in scenes():
        if a.only and name not in a.only.split(","):
            continue
        rs = render(name, usd, a.outdir, a.build, a.force)
        row = {"name": name, "doc": doc(usd)}
        for r, _ in RENDERERS:
            exr, sec, ex, gpu = rs[r]
            st = stats(exr, rs["karma"][0] if r != "karma" else None) if os.path.exists(exr) else None
            row[r], row[r + "_ex"], row[r + "_gpu"] = sec, ex, gpu
            row[r + "_stats"] = st
            row[r + "_mad"] = st[3] * 100 if st and st[3] is not None else None
        panel(name, rs)
        rows.append(row)
    for theme in COLOURS:
        with open(os.path.join(IMAGES, "times_%s.svg" % theme), "w") as f:
            f.write(bars(rows, [r for r, _ in RENDERERS], "Render time per scene, 512 x 512 (husk, wall clock)",
                         " s", "%.0f", theme))
        view = [dict(name=r["name"], eldr_default=r["eldr_default_mad"], eldr_final=r["eldr_final_mad"]) for r in rows]
        with open(os.path.join(IMAGES, "difference_%s.svg" % theme), "w") as f:
            f.write(bars(view, ["eldr_default", "eldr_final"],
                         "Difference to Karma: mean abs difference, exposure-matched, % of Karma's mean", "%", "%.0f",
                         theme))
    write_docs(rows)


def cell(st):
    if not st or st[1] is None:
        return "-"
    return "x%.2f, %.0f%% (%.0f%%)" % (st[1], st[2] * 100, st[3] * 100)


def table(rows, link):
    out = ["| Scene | Eldr default | Eldr final | Karma | Eldr default vs Karma | Eldr final vs Karma |",
           "|---|---:|---:|---:|---|---|"]
    for r in rows:
        t = ["%.1f s" % r[k] if r[k] is not None else "-" for k, _ in RENDERERS]
        for k in ("eldr_default", "eldr_final"):
            if r[k + "_gpu"] is not None:
                t[[x for x, _ in RENDERERS].index(k)] += " (%.1f s, %.1f ms)" % (r[k + "_ex"], r[k + "_gpu"])
        out.append("| %s | %s | %s | %s | %s | %s |" % (link(r["name"]), *t, cell(r["eldr_default_stats"]),
                                                       cell(r["eldr_final_stats"])))
    return "\n".join(out)


NOTE = ("Times are husk wall clock at 512 x 512, including startup and scene load; in brackets Eldr's time in "
        "its render pass and the GPU time of one film sample. "
        "*vs Karma* is the mean luminance ratio, then the mean absolute difference as rendered and, in brackets, "
        "after scaling the image to Karma's mean luminance (exposure-matched), both as a percentage of Karma's "
        "mean luminance.")


METHODOLOGY = ["## Methodology", "",
               "| | |", "|---|---|",
               "| Machine | Windows, NVIDIA GTX 1080 Ti (Eldr); Karma CPU on the same machine |",
               "| Resolution | 512 x 512, every scene |",
               "| Eldr default | The scene's `/Render/Settings` as authored, `husk -R HdVkRendererPlugin` |",
               "| Eldr final | The same with `tests/scenes/cornell/eldr_final.usda` sublayered over it |",
               "| Karma | `BRAY_HdKarma`, the scene's settings: 512 path-traced samples, 8 diffuse bounces |",
               "| Time | husk wall clock per run, startup and scene load included. In brackets (Eldr): time in its "
               "render pass and GPU time of one film sample, from `HDVK_TIMING=2` |",
               "| Brightness | Mean luminance against Karma's (x1.00: as bright as Karma) |",
               "| Difference | Mean absolute difference, % of Karma's mean luminance, as rendered and exposure-matched |",
               "| Regenerate | `python tests/render_gallery.py [outdir] [--only base,glossy] [--force]` "
               "(renders, pictures, this page and README.md's gallery) |", ""]


def caption(r):
    """Under each picture: every renderer's time, and Eldr's brightness against Karma."""
    out = []
    for k, label in RENDERERS:
        t = "%s %s" % (label, "%.1f s" % r[k] if r[k] is not None else "-")
        st = r.get(k + "_stats")
        if k != "karma" and st and st[1] is not None:
            t += ", brightness x%.2f" % st[1]
        out.append(t)
    return "; ".join(out)


def write_docs(rows):
    md = ["# Gallery", "",
          "Every scene is the CC0 Cornell box (`tests/scenes/cornell`) plus one small variant layer, rendered with "
          "husk at 512 x 512: Eldr with the scene's render settings as authored (*Eldr default*), Eldr with the final "
          "settings of `tests/scenes/cornell/eldr_final.usda` (*Eldr final*: ray-traced reflections, refraction and "
          "shadows, GI, final gather, caustics, denoising, 256 film samples) and Karma CPU with the scene's settings "
          "(512 path-traced samples, 8 diffuse bounces). `python tests/render_gallery.py` renders them again.", "",
          "![Render time per scene](images/gallery/times_light.svg)", "",
          "![Difference to Karma per scene](images/gallery/difference_light.svg)", "",
          table(rows, lambda n: "[%s](#%s)" % (n, n)), "", NOTE, ""] + METHODOLOGY
    for r in rows:
        md += ["## %s" % r["name"], "", r["doc"], ""] + (["*In Eldr:* " + NOTES[r["name"]], ""] if r["name"] in NOTES else [])
        md += ["![%s, left to right Eldr default, Eldr final, Karma. %s](images/gallery/%s.jpg)"
               % (r["name"], caption(r), r["name"]), ""]
    with open(os.path.join(REPO, "docs", "gallery.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(md))
    readme = os.path.join(REPO, "README.md")
    text = open(readme, encoding="utf-8").read()
    hero = ["## Gallery", "",
            "The CC0 Cornell box and one variant per feature, rendered with husk at 512 x 512: Eldr default, Eldr "
            "final and Karma side by side. [docs/gallery.md](docs/gallery.md) has each scene's purpose and how it is "
            "rendered.", "",
            picture("times", "Render time per scene for Eldr default, Eldr final and Karma"), "",
            picture("difference", "Mean absolute difference to Karma per scene for Eldr default and Eldr final"), "",
            table(rows, lambda n: "[%s](docs/gallery.md#%s)" % (n, n)), "", NOTE, ""]
    for r in rows:
        hero += ["![%s, left to right Eldr default, Eldr final and Karma: %s](docs/images/gallery/%s.jpg)"
                 % (r["name"], r["doc"].replace("[", "(").replace("]", ")"), r["name"]), ""]
    start = text.index("## Gallery")
    end = text.index("A few features, off and on:")
    with open(readme, "w", encoding="utf-8", newline="\n") as f:
        f.write(text[:start] + "\n".join(hero) + "\n" + text[end:])


if __name__ == "__main__":
    main()
