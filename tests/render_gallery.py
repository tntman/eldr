"""Render the gallery with Eldr and Karma and write docs/gallery.md, docs/images/gallery/ and README.md's
Gallery section.

  hython tests/render_gallery.py [outdir] [--only base,glossy] [--force] [--build <repo>/build-h22l]

Two sections. The Cornell box (tests/scenes/cornell: cornell.usda and every variants/*.usda, 512 x 512, then
the showcase's Cornell boxes lit by glowing surfaces, EMISSIVE) and the feature scenes (FEATURES, folders of
showcase/). Each scene renders four ways through husk:
  eldr_default  Eldr's effects without ray tracing: tests/scenes/cornell/eldr_default.usda over the
                scene's settings (/Render/Settings, or a showcase's /Render/eldr_default)
  eldr_final    ray traced: eldr_final.usda over /Render/Settings, or a showcase's /Render/eldr_final
  karma_xpu     BRAY_HdKarmaXPU with the scene's Karma settings (/Render/Settings or /Render/karma)
  karma_cpu     BRAY_HdKarma, the same: the brightness reference
Renders land in outdir (default %TEMP%/eldr_gallery) and are reused unless --force. A renderer that
writes no image (Karma XPU on a GPU it can't use) leaves its card out.
Times, from each renderer's own log, scene load left out of the render time and shown on its own:
  Eldr   renders twice, the first to warm its pipeline cache (shader compiles are a first-run cost, kept
         as "cold" in times.json). Render: its render passes (HDVK_TIMING=2, the sum of execute_ms) less
         the loading inside them ("Eldr: loaded in": shaders, textures, rt pipelines, BVH). Load: that line.
  Karma  husk -V2. Render: the frame's total wall clock less its load and update. Load: load (less husk's
         start) plus update.
Samples: Karma Path Traced at a fixed count (the Cornell box's 512, a showcase karma prim's, EMISSIVE_SAMPLES
without a light), Eldr its film
samples (FILM); both on the cards. The Eldr default log must not show a ray tracing pipeline compiling
("HdVk rt pipeline"); the script warns when it does.
Each scene's picture is one JPEG: a row of cards (render, renderer, resolution, samples, time,
brightness against Karma CPU) drawn with Houdini's hoiiotool. Brightness is the mean luminance ratio
from tests/compare_renderers.py. EXTRAS add pictures under three feature scenes: the AOV passes and the
Cryptomatte mattes (Eldr ray traced and Karma CPU), and tiles against the whole image with GPU memory by
resolution; their text is kept in docs/images/gallery/extras.json for runs that skip them.
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
from compare_renderers import EMISSIVE_SAMPLES, HUSK, has_lights, karma_layer, oiio, run, stats  # noqa: E402

# hython sets OCIO to Houdini's config, where hoiiotool's "linear" isn't the sRGB primaries' linear.
os.environ.pop("OCIO", None)
CORNELL = os.path.join(HERE, "scenes", "cornell")
SHOWCASE = os.path.join(REPO, "showcase")
IMAGES = os.path.join(REPO, "docs", "images", "gallery")
FILM = 256  # eldr:filmSamples of eldr_default.usda, eldr_final.usda and the showcases' eldr_final prims

RENDERERS = [("eldr_default", "Eldr raster + SSGI", "circle"), ("eldr_final", "Eldr ray traced", "diamond"),
             ("karma_xpu", "Karma XPU", "triangle"), ("karma_cpu", "Karma CPU", "square")]
LABEL = {r: l for r, l, _ in RENDERERS}
# A card's effects line (Karma's: its diffuse bounces and denoiser, read from the scene's settings).
EFFECTS = {"eldr_default": "SSGI, SSR, AO, screen-space refraction, subsurface, soft shadows, denoise",
           "eldr_final": "Traced reflections, refraction, shadows, GI, final gather, caustics, denoise"}
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
          "post_processing": "Post-processing", "displacement": "Displacement",
          "height_fog": "Height fog", "uv_sets": "UV sets", "glass": "Glass and caustics",
          "complex_caustics": "Complex caustics",
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
    "emissive_lights": "A glowing panel under the ceiling and a small bulb on the floor are the only lights: they light "
                       "the room and cast shadows from the box and the chrome ball.",
    "emissive_bounce": "A glowing panel lights the room; the walls pick up light and colour from each other.",
    "emissive_reflections": "A glowing panel and bulb light the room; the chrome ball reflects all of it, including "
                            "what is off screen.",
    "opacity_mode": "Two 30% opaque dark metal quads under a glowing panel: the left one \"transparent\" keeps its "
                    "highlight, the right one \"presence\" fades completely.",
}
# What a card shows in Eldr that differs from Karma on purpose or by a known limitation.
NOTES = {"complex_caustics": "Eldr raster + SSGI has no ray tracing and so no caustics: the casters' glass casts a "
                             "dark shadow, and screen-space GI misses the light bounced off the ceiling, out of view. "
                             "Eldr ray traced traces photons from the light, so the web and the glass's ring are "
                             "sharp where Karma's caustics, traced from the camera, are still noisy.",
         "opacity_mode": "the right quad fades with its opacity, as UsdPreviewSurface's \"presence\" mode asks; "
                         "Karma renders both quads alike, with the full highlight.",
         "materialx_nodes": "the dodge swatch (top row, fifth) follows MaterialX's own formula, as Storm does; "
                            "Karma's differs. Karma XPU leaves the glTF and Disney surfaces grey.",
         "bsdf_graphs": "Karma XPU leaves these surfaces grey: it doesn't render MaterialX BSDF graphs or Lama."}
# Showcase Cornell boxes lit by glowing surfaces only, in the Cornell section: (name, showcase folder, heading).
# Karma renders them at EMISSIVE_SAMPLES.
EMISSIVE = [("emissive_lights", "2026-10-28_03_emissive_lights", "Glowing panel and bulb"),
            ("emissive_bounce", "2026-10-28_04_gi_final_gather", "Bounce light from a glowing panel"),
            ("emissive_reflections", "2026-10-28_09_traced_reflections", "Chrome ball in a glowing room"),
            ("opacity_mode", "2026-10-29_01_opacity_mode", "Transparent and presence opacity")]
# Feature scenes: (name, showcase folder, heading). Their eldr_default prim gets eldr_default.usda.
FEATURES = [("material_lobes", "2026-10-30_22_material_lobes", "Material lobes"),
            ("subsurface", "2026-10-30_13_subsurface", "Skin, jade and leaves"),
            ("caustics", "2026-10-29_13_caustics", "Caustics"),
            ("reflections", "2026-11-04_01_reflections", "Reflections"),
            ("curves_points", "2026-10-30_19_curves_points", "Curves, points and hair"),
            ("light_shapes", "2026-10-30_07_light_shapes", "Light shapes"),
            ("many_lights", "2026-10-30_06_many_lights", "Many lights"),
            ("volume_scattering", "2026-10-29_26_volume_multiple_scattering", "Volume multiple scattering"),
            ("displacement", "2026-10-30_18_displacement", "Displacement shaders"),
            ("bsdf_graphs", "2026-10-31_05_bsdf_graphs", "MaterialX BSDF graphs"),
            ("materialx_nodes", "2026-10-29_16_materialx_nodes", "MaterialX and Karma nodes"),
            ("default_material", "2026-10-31_02_karma_default_material", "Objects without a material"),
            ("textures_colour", "2026-10-30_21_textures_colour", "Texture colour spaces and UDIMs"),
            ("refraction_quality", "2026-10-31_01_refraction_quality", "Rough glass"),
            ("transparency_order", "2026-10-29_09_transparency_order", "Glass, translucency and smoke in order"),
            ("planar_blur", "2026-10-29_03_planar_reflection_blur", "Rough mirror floor"),
            ("ssr", "2026-10-30_11_screen_space_reflections", "Screen-space reflections"),
            ("ambient_occlusion", "2026-10-30_09_ambient_occlusion", "Ambient occlusion"),
            ("shadow_modes", "2026-10-30_10_shadow_modes", "Shadows"),
            ("dome_sun", "2026-10-30_08_dome_sun", "Sun from an HDRI"),
            ("further_suns", "2026-10-30_26_further_sun_shadows", "A second sun's shadows"),
            ("shadow_linking", "2026-10-29_15_light_linking", "Light and shadow linking"),
            ("colour_limit", "2026-10-29_17_colour_limit", "Colour limit"),
            ("reflection_blur", "2026-10-29_10_reflection_motion_blur", "Motion blur in reflections"),
            ("instancing", "2026-10-30_20_instancing", "Nested instancing"),
            ("coloured_shadows", "2026-10-29_05_coloured_volume_shadows", "Coloured volume shadows"),
            ("volume_blur", "2026-10-29_18_volume_motion_blur", "Volume motion blur"),
            ("cryptomatte", "2026-10-30_15_cryptomatte", "Cryptomatte"),
            ("aovs", "2026-10-29_06_karma_aovs", "AOVs and light groups"),
            ("tiles", "2026-11-02_01_tiles", "Tiled rendering")]


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


def karma_effects(block):
    """Karma's effects line from its settings prim's text: path traced, diffuse bounces (its default 1), OIDN."""
    n = int(float(find(r"diffuselimit = ([\d.]+)", block, default=("1",))[0]))
    return "Path traced, %d diffuse bounce%s%s" % (n, "" if n == 1 else "s", ", OIDN" if "oidn" in block else "")


def showcase(name, folder, title, section):
    from pxr import Usd
    usd = os.path.join(SHOWCASE, folder, "scene.usda")
    karma = prim_block(open(usd, encoding="utf-8").read(), "karma")
    readme = open(os.path.join(SHOWCASE, folder, "README.md"), encoding="utf-8").read()
    summary = readme.split("<!-- summary -->")
    spp = int(find(r"pathtracedsamples = (\d+)", karma, default=("256",))[0])
    return dict(name=name, section=section, title=title, usd=usd, folder=folder,
                desc=DESCRIPTIONS.get(name) or (" ".join(summary[1].split()) if len(summary) > 2 else ""),
                res=tuple(map(int, find(r"resolution = \((\d+), (\d+)\)", karma, default=(960, 540)))),
                spp=spp if has_lights(Usd.Stage.Open(usd)) else max(spp, EMISSIVE_SAMPLES),
                karma_effects=karma_effects(karma),
                frame=int(find(r"Render frame (\d+)", readme, default=("1",))[0]),  # the README's frame
                prims=dict(eldr_default="/Render/eldr_default", eldr_final="/Render/eldr_final", karma="/Render/karma"))


def scenes():
    """[{name, section, title, desc, usd, folder (showcases), prims: {renderer: settings prim}, res, spp}]."""
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
                        karma_effects=karma_effects(prim_block(base, "Settings")),
                        prims=dict(eldr_default="/Render/Settings", eldr_final="/Render/Settings",
                                   karma="/Render/Settings")))
    out += [showcase(name, folder, title, "cornell") for name, folder, title in EMISSIVE]
    out += [showcase(name, folder, title, "features") for name, folder, title in FEATURES]
    return out


def karma_times(log):
    """(render, load) seconds from Karma's husk -V2 log. Its clocks: Start and Load since husk started,
    Update how long the update took, Total when the frame finished."""
    t = open(log, errors="replace").read() if os.path.exists(log) else ""

    def last(what):
        m = re.findall(r"%s Wall Clock Time: (\d+):(\d+):(\d+(?:\.\d+)?)" % what, t)
        return int(m[-1][0]) * 3600 + int(m[-1][1]) * 60 + float(m[-1][2]) if m else 0.0
    total, start, load, update = last("Total"), last("Start"), last("Load"), last("Update")
    return (total - load - update, load - start + update) if total else (None, None)


def eldr_times(log, csv):
    """(render, load) seconds from Eldr's log and timing CSV: its render passes less the loading in them
    (the textures they collect, the CSV's collect_ms; shaders, rt pipelines and BVH from the log; sync and
    the textures read before runs outside them), and all its loading."""
    rows = list(csvfile.DictReader(open(csv))) if os.path.exists(csv) else []
    if not rows:
        return None, None
    loads = re.findall(r"Eldr: loaded in ([\d.]+) s: shaders ([\d.]+), textures [\d.]+, rt pipelines ([\d.]+), "
                       r"BVH ([\d.]+)", open(log, errors="replace").read())
    load = sum(float(a) for a, *_ in loads)
    inside = sum(float(s) + float(r) + float(b) for _, s, r, b in loads) + \
        sum(float(x.get("collect_ms") or 0) for x in rows) / 1000
    return max(sum(float(x["execute_ms"]) for x in rows) / 1000 - inside, 0.0), load


def timed(cmd, log, csv, env, runner=run):
    """Runs a husk command (its -o, then the scene, last) the way the gallery and the production scenes
    time it: Eldr renders once first to warm its pipeline cache (shader compiles are a first-run cost; that
    run's load is kept as cold). (runner's result, render s, load s, cold load s); render and load from the
    renderer's own log (eldr_times, karma_times)."""
    eldr = "HdVkRendererPlugin" in cmd
    cold = None
    if eldr:
        o = cmd.index("-o")
        warm = [csv[:-4] + "_cold" + ext for ext in (".exr", ".log", ".csv")]
        run(cmd[:o + 1] + [warm[0]] + cmd[o + 2:], warm[1], dict(env, HDVK_TIMING_CSV=warm[2]))
        cold = eldr_times(warm[1], warm[2])[1]
        if os.path.exists(warm[0]):
            os.remove(warm[0])
    result = runner(cmd, log, dict(env, HDVK_TIMING_CSV=csv))
    sec, load = eldr_times(log, csv) if eldr else karma_times(log)
    return result, sec, load, cold


def render(s, outdir, build, force, new):
    """{renderer: {exr, sec, wall, rt}} for one scene; a renderer without an image is left out. Renders
    only when `new` (else uses what earlier runs left)."""
    d = os.path.join(outdir, s["section"], s["name"])
    os.makedirs(d, exist_ok=True)
    usd, prims = s["usd"], s["prims"]
    if "folder" not in s:
        default = layer(os.path.join(d, "default_layer.usda"), usd, sublayer=os.path.join(CORNELL, "eldr_default.usda"))
        final = layer(os.path.join(d, "final_layer.usda"), usd, sublayer=os.path.join(CORNELL, "eldr_final.usda"))
    karma = usd
    if "folder" in s:
        default = layer(os.path.join(d, "default_layer.usda"), usd,
                        reference=("eldr_default", os.path.join(CORNELL, "eldr_default.usda")))
        final = usd
        # Karma: domes shown to the camera as Eldr shows them, beauty only (a showcase's products carry
        # Eldr's vars), Path Traced at the karma prim's samples (Karma's default Variance mode stops early).
        from pxr import Usd
        karma = karma_layer(Usd.Stage.Open(usd), usd, os.path.join(d, "karma_layer.usda"), True, prims["karma"],
                            s["spp"])
    env = husk_env(outdir, build)
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
            (code, wall), sec, load, cold = timed(
                [HUSK, "-R", plugin, "-s", prim, "-f", str(s.get("frame", 1)), "-n", "1", "-V", "2", "-o", exr, scene],
                log, csv, env)
            rt = "HdVk rt pipeline" in open(log, errors="replace").read()
            known[r] = dict(ok=os.path.exists(exr), wall=wall, sec=sec if sec is not None else wall, load=load,
                            cold=cold, rt=rt)
            print("%-9s %-18s %-13s %s %.2f s, load %s (wall %.1f s)%s" % (
                s["section"], s["name"], r, "ok" if known[r]["ok"] else "FAILED (%d)" % code, known[r]["sec"],
                "-" if load is None else "%.2f s" % load + ("" if cold is None else ", cold %.1f s" % cold), wall,
                "  WARNING: ray tracing started" if rt and r == "eldr_default" else ""), flush=True)
            json.dump(known, open(times, "w"))
        if known.get(r, {}).get("ok"):
            out[r] = dict(known[r], exr=exr)
    return out


def duration(sec):
    return "%.0f ms" % (sec * 1000) if sec < 1 else "%.1f s" % sec


def load_time(sec):
    """A load figure: Eldr logs its load to 0.1 s."""
    return "< 0.1 s" if sec < 0.05 else duration(sec)


def sampling(r, spp):
    return "%d film samples" % FILM if r.startswith("eldr") else "%d spp" % spp


def brightness(row, r):
    if r == "karma_cpu":
        return "Brightness reference"
    v = row.get(r + "_ratio")
    return "Brightness %.2f× Karma CPU" % v if v is not None else "Brightness -"


def hexrgb(h):
    return ",".join("%.3f" % (int(h[i:i + 2], 16) / 255) for i in (1, 3, 5))


def card(src, w, h, r, label, lines, out):
    """One card: `src`'s RGB (linear) at w x h on the dark card, renderer r's colour bar and glyph, `label`,
    then `lines` (the first bright, the rest dim)."""
    cw, base = w + 16, 12 + h
    ch = base + 42 + 21 * len(lines)
    text = []
    for i, line in enumerate(lines):
        text += ["--text:x=10:y=%d:size=%d:color=%s:font=segoeui" % (
            base + 54 + 21 * i, 14 if i == 0 and len(line) > 64 else 15, CARD["text"] if i == 0 else CARD["dim"]), line]
    oiio(src, "--ch", "R,G,B", "--croptofull", "--resize", "%dx%d" % (w, h), "--clamp:min=0",
         "--colorconvert", "linear", "sRGB", "--clamp:min=0:max=1",
         "--create", "%dx%d" % (cw, ch), "3", "--fill:color=" + CARD["bg"], "%dx%d+0+0" % (cw, ch),
         "--fill:color=" + hexrgb(COLOURS["dark"][r]), "%dx4+0+0" % cw, "--paste", "+8+12",
         "--text:x=10:y=%d:size=18:color=%s:font=seguisym" % (base + 30, hexrgb(COLOURS["dark"][r])),
         GLYPH[next(m for k, _, m in RENDERERS if k == r)],
         "--text:x=32:y=%d:size=18:color=%s:font=seguisb" % (base + 30, CARD["text"]), label, *text, "-o", out)
    return out


def mosaic(cards, cols, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    oiio(*cards, "--mosaic:pad=12", "%dx%d" % (cols, (len(cards) + cols - 1) // cols), "-d", "uint8",
         "--attrib", "Compression", "jpeg:85", "-o", path)


def panel(row, rs, path):
    """The scene's cards side by side as one JPEG."""
    w, h = 512, round(512 * row["res"][1] / row["res"][0])
    cards = []
    for r, label, _ in RENDERERS:
        if r not in rs:
            continue
        line1 = "%d × %d · %s · %s%s" % (*row["res"], sampling(r, row["spp"]), duration(rs[r]["sec"]),
                                         "" if rs[r].get("load") is None else " + %s load" % load_time(rs[r]["load"]))
        cards.append(card(rs[r]["exr"], w, h, r, label, [EFFECTS.get(r, row["karma_effects"]), line1,
                                                         brightness(row, r)], rs[r]["exr"][:-4] + "_card.png"))
    mosaic(cards, len(cards), path)


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


def husk_env(outdir, build):
    env = dict(os.environ, MSYS_NO_PATHCONV="1", HOUDINI_PACKAGE_DIR=os.path.join(build, "houdini"),
               PXR_PLUGINPATH_NAME=build, HDVK_TIMING="2")
    env.setdefault("HOUDINI_USER_PREF_DIR", os.path.join(outdir, "hprefs__HVER__"))
    return env


def husk_once(plugin, prim, scene, exr, env, force, *args):
    """One husk render to exr unless it is there (and not force); (log, wall s, peak GPU MB or None)."""
    from render_production import husk  # run() with nvidia-smi sampling
    log = exr[:-4] + ".log"
    if force or not os.path.exists(exr[:-4] + ".json"):  # the json stays when a huge EXR is deleted
        code, wall, mem = husk([HUSK, "-R", plugin, "-s", prim, "-f", "1", "-n", "1", "-V", "2", *args, "-o", exr,
                                scene], log, dict(env, HDVK_TIMING_CSV=exr[:-4] + ".csv"))
        json.dump(dict(wall=wall, mem=mem), open(exr[:-4] + ".json", "w"))
        print("  %s %s: %s, %.1f s, %s MB" % (os.path.basename(exr), plugin, "ok" if os.path.exists(exr) else
                                               "FAILED (%d)" % code, wall, mem), flush=True)
    return json.load(open(exr[:-4] + ".json")) if os.path.exists(exr[:-4] + ".json") else {}


def exr_channels(exr):
    """(channel names, {name: h x w float array}) over every part of a (multi-part) EXR."""
    import OpenImageIO as oi
    names, px = [], {}
    part = 0
    while True:
        buf = oi.ImageBuf(exr, part, 0)
        if buf.has_error or part >= max(1, buf.nsubimages):
            return names, px
        spec = buf.spec()
        a = buf.get_pixels(oi.FLOAT).reshape(spec.height, spec.width, spec.nchannels)
        for i, c in enumerate(spec.channelnames):
            names.append(c)
            px[c] = a[:, :, i]
        part += 1


def channels(exr, names):
    """{name: h x w x 3 float array} of an EXR's layers (the beauty as "C": R, G, B); a 1-channel layer
    is repeated, a missing one left out."""
    import numpy as np
    cn, px = exr_channels(exr)
    out = {}
    for n in names:
        if n == "C":
            got = [c for c in ("R", "G", "B") if c in cn]
        else:
            got = [c for c in cn if c == n or c.startswith(n + ".") and c[len(n) + 1:].upper() in "RGBXYZ"]
            got = got[:3] if len(got) >= 3 else got[:1] * 3
        if got:
            out[n] = np.stack([px[c] for c in got], -1)
    return out


def write_rgb(path, rgb):
    import numpy as np
    import OpenImageIO as oi
    buf = oi.ImageBuf(oi.ImageSpec(rgb.shape[1], rgb.shape[0], 3, oi.FLOAT))
    buf.set_pixels(oi.ROI(), np.ascontiguousarray(rgb, dtype=np.float32))
    buf.write(path)
    return path


def matte_colours(exr, layer):
    """A Cryptomatte layer's mattes as colours: each rank's id hashed to a colour, weighted by its coverage
    (linear, for card()); None without the layer."""
    import numpy as np
    cn, px = exr_channels(exr)
    ranks = sorted((c for c in cn if re.match(r"%s\d\d\.[RGBArgba]$" % layer, c)),
                   key=lambda c: (c[:-2], "RGBA".index(c[-1].upper())))
    if not ranks:
        return None
    out = np.zeros(px[ranks[0]].shape + (3,), np.float32)
    for i in range(0, len(ranks), 2):  # (id, coverage) pairs: R,G then B,A of each 00, 01, 02 layer
        ids, cov = px[ranks[i]], px[ranks[i + 1]]
        h = (ids.astype(np.float32).view(np.uint32).astype(np.uint64) * 2654435761) & 0xffffffff
        col = np.stack([((h >> s) & 255) / 255.0 for s in (0, 8, 16)], -1) * 0.8 + 0.2
        out += np.where(ids[..., None] != 0, col ** 2.2, 0) * cov[..., None]
    return out


AOV_VARS = '''
over "Render"
{
    over "aovs"
    {
        append rel orderedVars = [</Render/Vars/combineddiffuse>, </Render/Vars/combinedglossyreflection>,
                                  </Render/Vars/albedo>, </Render/Vars/N>, </Render/Vars/depth>]
    }
    over "karma"
    {
        token karma:global:convergence_mode = "Path Traced"
    }
    over "Vars"
    {
%s
    }
}
'''


def aov_var(name, dtype, fmt, kind, source):
    return ('        def RenderVar "%s" (apiSchemas = ["HuskRenderVarAPI"])\n        {\n'
            '            uniform token dataType = "%s"\n            token driver:parameters:aov:husk:format = "%s"\n'
            '            string driver:parameters:aov:husk:name = "%s"\n'
            '            uniform string sourceName = "%s"\n            uniform token sourceType = "%s"\n        }'
            % (name, dtype, fmt, name, source, kind))


# AOV grid: (var, label, how it shows: "light" at the beauty's exposure, "normal", "depth", "albedo")
PASSES = [("C", "Beauty", "light"), ("combineddiffuse", "Diffuse", "light"),
          ("combinedglossyreflection", "Specular", "light"), ("glossytransmission", "Transmission", "light"),
          ("beauty_ceiling", "Light group: ceiling", "light"), ("beauty_lamp", "Light group: lamp", "light"),
          ("N", "Normal", "normal"), ("depth", "Depth", "depth"), ("albedo", "Albedo", "albedo")]


def extra_aovs(s, d, env, force):
    """The AOV showcase's passes, Eldr ray traced next to Karma CPU, a grid of card pairs."""
    import numpy as np
    wrap = os.path.join(d, "aov_layer.usda")
    with open(wrap, "w", newline="\n") as f:
        f.write('#usda 1.0\n(\n    subLayers = [@%s@]\n)\n' % fwd(s["usd"]) + AOV_VARS % "\n".join([
            aov_var("combineddiffuse", "color3f", "half3", "lpe", "C<RD>.*L"),
            aov_var("combinedglossyreflection", "color3f", "half3", "lpe", "C<RG>.*L"),
            aov_var("albedo", "color3f", "half3", "lpe", "CDA"),
            aov_var("N", "normal3f", "float3", "raw", "ray:hitN"),
            aov_var("depth", "float", "float", "raw", "ray:hitPz")]))
    exrs = {"eldr_final": os.path.join(d, "aovs_eldr.exr"), "karma_cpu": os.path.join(d, "aovs_karma.exr")}
    husk_once("HdVkRendererPlugin", "/Render/eldr_final", wrap, exrs["eldr_final"], env, force)
    husk_once("BRAY_HdKarma", "/Render/karma", wrap, exrs["karma_cpu"], env, force)
    layers = {r: channels(e, [p for p, _, _ in PASSES]) for r, e in exrs.items() if os.path.exists(e)}
    if "eldr_final" not in layers:
        return []
    depth = layers["eldr_final"].get("depth")
    near, far = (np.percentile(depth[depth > 0], [1, 99]) if depth is not None and (depth > 0).any() else (0, 1))
    cards = []
    w = 300
    for var, label, how in PASSES:
        for r in ("eldr_final", "karma_cpu"):
            img = layers.get(r, {}).get(var)
            name = "%s: %s" % ("Eldr" if r == "eldr_final" else "Karma CPU", label)
            if img is None:
                img, name = np.zeros((s["res"][1], s["res"][0], 3), np.float32), name + " (not written)"
            elif how == "normal":
                img = np.clip(img * 0.5 + 0.5, 0, 1) ** 2.2 * (np.abs(img).sum(-1, keepdims=True) > 0)
            elif how == "depth":
                img = np.where(img > 0, np.clip(1 - (img - near) / max(far - near, 1e-6), 0, 1), 0) ** 2.2
            cards.append(card(write_rgb(os.path.join(d, "aov_%s_%s.exr" % (r, var)), img), w, w, r, name, [],
                              os.path.join(d, "aov_%s_%s_card.png" % (r, var))))
    mosaic(cards, 6, os.path.join(IMAGES, "features", "aovs_passes.jpg"))
    return ["Its passes, Eldr ray traced (left of each pair) next to Karma CPU (right), from one EXR each: the "
            "diffuse and specular splits (`C<RD>.*L`, `C<RG>.*L`), the light refracted through the glass ball "
            "(`C<TG>.*L`), the two light groups (`C.*<L.'ceiling'>`, `C.*<L.'lamp'>`), normal (`ray:hitN`, shown as "
            "0.5 + N / 2), depth (`ray:hitPz`, near is bright) and albedo (`CDA`). Eldr's light groups hold the "
            "group's direct light; Karma's add its bounced light.", "",
            "![AOVs: beauty, diffuse, specular, transmission, light groups, normal, depth, albedo; Eldr and Karma CPU]"
            "({img}features/aovs_passes.jpg)", ""]


def extra_cryptomatte(s, d, env, force):
    """Cryptomatte: the beauty and the object and material mattes as colours, Eldr ray traced and Karma CPU."""
    wrap = os.path.join(d, "crypto_karma.usda")
    with open(wrap, "w", newline="\n") as f:
        f.write('#usda 1.0\n(\n    subLayers = [@%s@]\n)\nover "Render"\n{\n    over "karma"\n    {\n'
                '        token karma:global:convergence_mode = "Path Traced"\n        int karma:global:pathtracedsamples = 64\n'
                '    }\n    over "Vars"\n    {\n%s\n    }\n}\n' % (fwd(s["usd"]), "\n".join(
                    '        over "%s"\n        {\n            bool driver:parameters:aov:karma:cryptomatte = 1\n'
                    '            int driver:parameters:aov:karma:cryptomatterank = 6\n        }' % v
                    for v in ("CryptoObject", "CryptoMaterial"))))
    karma = os.path.join(d, "crypto_karma.exr")
    husk_once("BRAY_HdKarma", "/Render/karma", wrap, karma, env, force)
    w, h = 512, round(512 * s["res"][1] / s["res"][0])
    cards = []
    for r, exr in (("eldr_final", os.path.join(d, "eldr_final.exr")), ("karma_cpu", karma)):
        who = "Eldr ray traced" if r == "eldr_final" else "Karma CPU"
        if not os.path.exists(exr):
            continue
        cards.append(card(exr, w, h, r, who + ": beauty", [], os.path.join(d, "crypto_%s_beauty_card.png" % r)))
        for layer, what in (("CryptoObject", "objects"), ("CryptoMaterial", "materials")):
            m = matte_colours(exr, layer)
            if m is not None:
                cards.append(card(write_rgb(os.path.join(d, "crypto_%s_%s.exr" % (r, layer)), m), w, h, r,
                                  "%s: %s" % (who, what), [], os.path.join(d, "crypto_%s_%s_card.png" % (r, layer))))
    if not cards:
        return []
    mosaic(cards, 3, os.path.join(IMAGES, "features", "cryptomatte_mattes.jpg"))
    return ["The Cryptomatte layers of the same EXRs, each id a colour weighted by its coverage: soft, anti-aliased "
            "edges where objects meet. Eldr names objects by prim path (`CryptoObject`) and materials by material "
            "path (`CryptoMaterial`), as Karma's Cryptomatte does.", "",
            "![Cryptomatte: beauty, object and material mattes; Eldr and Karma CPU]"
            "({img}features/cryptomatte_mattes.jpg)", ""]


# Tiled rendering: peak GPU memory and time by resolution, tiles of TILE pixels against the whole image.
TILE = 2048
TILE_RUNS = [(1920, 1080, True), (4096, 4096, True), (8192, 8192, True), (12000, 12000, True),
             (1920, 1080, False), (4096, 4096, False)]


def extra_tiles(s, d, env, force):
    """Tiles against the whole image (and their difference), and GPU memory by resolution."""
    tiles, whole = os.path.join(d, "tiles.exr"), os.path.join(d, "whole.exr")
    husk_once("HdVkRendererPlugin", "/Render/tiles", s["usd"], tiles, env, force)
    husk_once("HdVkRendererPlugin", "/Render/whole", s["usd"], whole, env, force)
    if not (os.path.exists(tiles) and os.path.exists(whole)):
        return []
    diff = os.path.join(d, "tiles_diff.exr")
    oiio(tiles, "--ch", "R,G,B", whole, "--ch", "R,G,B", "--absdiff", "--mulc", "10,10,10", "-o", diff)
    st = stats(tiles, whole)
    w, h = 640, 360
    cards = [card(tiles, w, h, "eldr_default", "512-pixel tiles (12 tiles)", [], os.path.join(d, "tiles_card.png")),
             card(whole, w, h, "eldr_default", "Whole image", [], os.path.join(d, "whole_card.png")),
             card(diff, w, h, "eldr_default", "Difference × 10", ["Mean %.2f%% of the image's brightness" % (st[2] * 100)],
                  os.path.join(d, "diff_card.png"))]
    mosaic(cards, 3, os.path.join(IMAGES, "features", "tiles_whole.jpg"))
    wrap = os.path.join(d, "tile_layer.usda")
    with open(wrap, "w", newline="\n") as f:
        f.write('#usda 1.0\n(\n    subLayers = [@%s@]\n)\nover "Render"\n{\n    over "tiles"\n    {\n'
                '        custom int eldr:tileSize = %d\n    }\n}\n' % (fwd(s["usd"]), TILE))
    rows = ["| Image | Rendered | Peak GPU memory | Time |", "|---|---|---:|---:|"]
    for rw, rh, tiled in TILE_RUNS:
        exr = os.path.join(d, "mem_%dx%d_%s.exr" % (rw, rh, "tiles" if tiled else "whole"))
        m = husk_once("HdVkRendererPlugin", "/Render/" + ("tiles" if tiled else "whole"), wrap, exr, env, force,
                      "--res", str(rw), str(rh))
        if m.get("wall") is not None:
            sec, _ = eldr_times(exr[:-4] + ".log", exr[:-4] + ".csv")
            rows.append("| %d × %d | %s | %s | %s |" % (
                rw, rh, "%d-pixel tiles" % TILE if tiled else "whole", "-" if not m.get("mem") else
                "%.1f GB" % (m["mem"] / 1024), duration(sec if sec is not None else m["wall"])))
        if os.path.exists(exr) and rw > 4096:
            os.remove(exr)  # 12000 x 12000 is 2 GB of EXR
    return ["The tiled image (12 tiles of 512 pixels) against the whole image at once, and their difference "
            "ten times brighter: no seams along the tile edges, only a little noise and the chrome ball's "
            "reflection, which screen-space reflections find on screen in the whole image and the reflection "
            "probe supplies in a tile (traced reflections are the same either way).", "",
            "![Tiled rendering: tiles, whole image and their difference]({img}features/tiles_whole.jpg)", "",
            "The same scene (ray tracing off) at growing sizes with its `tiles` settings and %d-pixel tiles "
            "(peak GPU memory above what was in use before the render, sampled by nvidia-smi). The screen-size "
            "buffers, about 170 bytes a pixel, stay at the tile's size; only the whole image's bloom, exposure "
            "and tonemapping grow with it, about 27 bytes a pixel. A whole 12000 × 12000 image would need "
            "about 24 GB." % TILE, ""] + rows + [""]


EXTRAS = {"aovs": extra_aovs, "cryptomatte": extra_cryptomatte, "tiles": extra_tiles}
EXTRAS_JSON = os.path.join(IMAGES, "extras.json")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("outdir", nargs="?", default=os.path.join(tempfile.gettempdir(), "eldr_gallery"))
    p.add_argument("--only")
    p.add_argument("--force", action="store_true")
    p.add_argument("--build", default=os.path.join(REPO, "build-h22l"))
    a = p.parse_args()
    os.makedirs(IMAGES, exist_ok=True)
    extras = json.load(open(EXTRAS_JSON)) if os.path.exists(EXTRAS_JSON) else {}
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
            row[r + "_load"] = v.get("load")
            st = stats(v["exr"], ref) if r != "karma_cpu" else None
            row[r + "_ratio"] = st[1] if st else None
            row[r + "_rt"] = v["rt"]
        if new:
            panel(row, rs, os.path.join(IMAGES, row["image"]))
            if s["name"] in EXTRAS:
                try:
                    extras[s["name"]] = EXTRAS[s["name"]](s, os.path.join(a.outdir, s["section"], s["name"]),
                                                          husk_env(a.outdir, a.build), a.force)
                    json.dump(extras, open(EXTRAS_JSON, "w"), indent=1)
                except Exception:  # a long run goes on; the scene keeps its last extras
                    import traceback
                    traceback.print_exc()
        row["extra"] = extras.get(s["name"], [])
        rows.append(row)
    charts(rows)
    write_docs(rows)
    for row in rows:
        if row.get("eldr_default_rt"):
            print("WARNING: Eldr raster + SSGI started ray tracing in", row["name"])
        missing = [LABEL[r] for r, _, _ in RENDERERS if r not in row["renderers"]]
        if missing:
            print("no image:", row["name"], ", ".join(missing))


def table(rows, link):
    keys = [r for r, _, _ in RENDERERS]
    out = ["| Scene | Samples (Eldr / Karma) | %s | Brightness vs Karma CPU (raster / ray traced / XPU) |"
           % " | ".join(LABEL[k] for k in keys), "|---|---|" + "---:|" * len(keys) + "---|"]
    for r in rows:
        t = ["-" if r.get(k + "_sec") is None else duration(r[k + "_sec"]) + (
            "" if r.get(k + "_load") is None else " <sub>+ %s</sub>" % load_time(r[k + "_load"])) for k in keys]
        b = " / ".join("%.2f×" % r[k + "_ratio"] if r.get(k + "_ratio") else "-" for k in keys[:3])
        out.append("| %s | %d / %d | %s | %s |" % (link(r), FILM, r["spp"], " | ".join(t), b))
    return "\n".join(out)


def slug(title):
    return re.sub(r"[^\w\- ]", "", title.lower()).replace(" ", "-")


HARDWARE = ("AMD Ryzen 9 3950X (16 cores, 32 threads), 48 GB RAM, NVIDIA GeForce GTX 1080 Ti 11 GB (driver 582.78; "
            "no RT cores, so Eldr's ray tracing runs through its compute fallback), Windows 11 Pro, Houdini 22.0.466. "
            "Karma CPU runs on the Ryzen, Karma XPU on the GTX 1080 Ti (with the CPU).")


def hardware():
    """The machine, read now (CPU from the registry, RAM, GPU and driver from nvidia-smi, Windows, Houdini
    from husk's path); HARDWARE when any of it can't be read."""
    import ctypes
    import platform
    import subprocess
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as k:
            cpu = " ".join(winreg.QueryValueEx(k, "ProcessorNameString")[0].split())
        cpu = re.sub(r" \d+-Core Processor$", "", cpu)

        class Mem(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [
                (n, ctypes.c_ulonglong) for n in ("total", "avail", "pagetotal", "pageavail", "virt", "virtavail", "ext")]
        m = Mem(length=ctypes.sizeof(Mem))
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        gpu, mib, driver = [s.strip() for s in subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.total,driver_version", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=30).stdout.splitlines()[0].split(",")]
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion") as k:
            edition = winreg.QueryValueEx(k, "EditionID")[0]
        houdini = re.search(r"Houdini (\d+\.\d+\.\d+)", HUSK).group(1)
    except Exception:
        return HARDWARE
    cores = "16 cores, " if "3950X" in cpu else ""  # ponytail: physical cores only known for this CPU
    rt = "" if "RTX" in gpu else "; no RT cores, so Eldr's ray tracing runs through its compute fallback"
    win = "Windows %s %s" % ("11" if int(platform.version().split(".")[-1]) >= 22000 else "10",
                             "Pro" if edition.startswith("Professional") else edition)
    return ("%s (%s%d threads), %.0f GB RAM, %s %.0f GB (driver %s%s), %s, Houdini %s. Karma CPU runs on the "
            "%s, Karma XPU on the %s (with the CPU)." % (
                cpu, cores, os.cpu_count(), m.total / 2 ** 30, gpu, int(mib) / 1024, driver, rt, win, houdini,
                cpu.split()[1] if " " in cpu else "CPU", gpu.replace("NVIDIA GeForce ", "")))


INTRO = ("Every scene renders four ways with husk: "
         "● **Eldr raster + SSGI**, every effect that works without ray tracing (screen-space GI, reflections and "
         "refraction, planar mirrors, ambient occlusion, subsurface, soft shadow maps, denoising, 256 film samples: "
         "`tests/scenes/cornell/eldr_default.usda`); "
         "◆ **Eldr ray traced** (reflections, refraction, shadows, GI, final gather, caustics, denoising, "
         "256 film samples: `tests/scenes/cornell/eldr_final.usda`); "
         "▲ **Karma XPU** and ■ **Karma CPU** with the scene's Karma settings. Karma CPU is the brightness "
         "reference.")

METHODOLOGY = ["### Methodology", "",
               "| | |", "|---|---|",
               "| Hardware | %s |",
               "| Eldr raster + SSGI | `tests/scenes/cornell/eldr_default.usda` over the scene's settings (a showcase's "
               "`eldr_default` prim), `husk -R HdVkRendererPlugin`; no ray tracing starts |",
               "| Eldr ray traced | `tests/scenes/cornell/eldr_final.usda` over `/Render/Settings`, or a showcase's "
               "`eldr_final` prim |",
               "| Karma | `BRAY_HdKarmaXPU` and `BRAY_HdKarma` with the scene's settings (Cornell box: 512 path-traced "
               "samples, 8 diffuse bounces; showcases: their `karma` prim, Path Traced, beauty only, domes visible to the camera as in Eldr) |",
               "| Samples | Karma: Path Traced at a fixed count, the same every pixel (Cornell box 512, showcases 256, "
               "scenes lit only by glowing surfaces %d), "
               "denoised with OIDN on the showcases. Eldr: 256 film samples, all taken (adaptive sampling off), "
               "denoised with OIDN. Each card shows its count |",
               "| Render time | From each renderer's own log, scene load left out. Eldr: its render passes "
               "(`HDVK_TIMING=2`) less the loading inside them. Karma: the frame's wall clock less load and update "
               "(`husk -V2`). Both include their denoise |",
               "| Load | The smaller figure after each time (+): Eldr's sync, shader compiles, texture loads, ray "
               "tracing pipelines and BVH builds; Karma's load and update. husk's own start-up is in neither |",
               "| Warm cache | Eldr renders each scene twice and times the second: its Vulkan pipeline cache keeps "
               "compiled shaders between runs, and a scene's first run compiles them (up to 8 s here, by its "
               "materials and features) |",
               "| Brightness | Mean luminance against Karma CPU's (1.00×: as bright as Karma CPU) |",
               "| Regenerate | `hython tests/render_gallery.py [outdir] [--only base,glossy] [--force]` "
               "(renders, pictures, this page and README.md's gallery) |", ""]
METHODOLOGY = [line.replace("%d", str(EMISSIVE_SAMPLES)) for line in METHODOLOGY]


def sections(rows, img, hero=None):
    """The two scene sections: a heading, a description and the cards' picture per scene."""
    md = []
    for sec, head, intro in [
            ("cornell", "Cornell box: one feature per scene",
             "The CC0 Cornell box (`tests/scenes/cornell`) plus one small variant layer, rendered at 512 × 512, "
             "then the showcase's Cornell boxes lit only by glowing surfaces (Karma at %d samples)." % EMISSIVE_SAMPLES),
            ("features", "Feature scenes",
             "Showcase scenes (`showcase/`), one feature each, rendered at their own resolution.")]:
        md += ["## " + head, "", intro, ""]
        if sec == "features" and hero:
            md += hero
        for r in rows:
            if r["section"] != sec:
                continue
            md += ["### " + r["title"], "", r["desc"] + (" (`showcase/%s`)" % r["folder"] if "folder" in r else ""), ""]
            if r["name"] in NOTES:
                md += ["*Note:* " + NOTES[r["name"]][0].upper() + NOTES[r["name"]][1:], ""]
            md += ["![%s: %s](%s%s)" % (r["title"], ", ".join(LABEL[k] for k in r["renderers"]), img, r["image"]), ""]
            md += [line.replace("{img}", img) for line in r.get("extra", [])]
    return md


def picture(stem, alt, prefix):
    return ('<picture>\n  <source media="(prefers-color-scheme: dark)" srcset="%s%s_dark.svg">\n'
            '  <img alt="%s" src="%s%s_light.svg" width="100%%">\n</picture>' % (prefix, stem, alt, prefix, stem))


def write_docs(rows):
    hw = hardware()
    md = ["# Gallery", "", "*Hardware: %s*" % hw, "", INTRO, "",
          "![Render time per scene](images/gallery/times_light.svg)", "",
          "![Brightness relative to Karma CPU per scene](images/gallery/brightness_light.svg)", "",
          table(rows, lambda r: "[%s](#%s)" % (r["title"], slug(r["title"]))), ""] + [
          line.replace("%s", hw) if line.startswith("| Hardware") else line for line in METHODOLOGY]
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
    gallery = ["## Gallery", "", "*Hardware: %s*" % hw, "", INTRO, "",
               picture("times", "Render time per scene for each renderer", "docs/images/gallery/"), "",
               picture("brightness", "Mean brightness relative to Karma CPU per scene", "docs/images/gallery/"), "",
               table(rows, lambda r: "[%s](docs/gallery.md#%s)" % (r["title"], slug(r["title"]))), "",
               "[docs/gallery.md](docs/gallery.md) has the methodology.", ""]
    gallery += ["#" + line if line.startswith("##") else line for line in sections(rows, "docs/images/gallery/", hero)]
    start = text.index("## Gallery")
    end = text.index("A few features, off and on")
    with open(readme, "w", encoding="utf-8", newline="\n") as f:
        f.write(text[:start] + "\n".join(gallery) + "\n" + text[end:])


if __name__ == "__main__":
    main()
