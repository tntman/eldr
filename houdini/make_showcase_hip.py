r"""Builds showcase/<folder>/scene.hip for every showcase (or the folders given):

    hython houdini/make_showcase_hip.py [showcase/<folder> ...]

Run with Eldr's Houdini package (HOUDINI_PACKAGE_DIR=<build-h22>\houdini,
PXR_PLUGINPATH_NAME=<build-h22>) so the Eldr tab of the Render Settings LOP
exists. For each folder it

- writes the standard RenderSettings prims into scene.usda (between the
  "standard settings" markers, rewritten every run): eldr_default (Eldr's
  defaults), eldr_final (every setting of FINAL ticked) and karma (Karma to
  match: same camera, resolution, samples, products, Karma's twins);
- builds scene.hip: a Sublayer LOP on $HIP/scene.usda, one render settings
  node per settings prim (the standard three, then the scene's own A/B prims
  as Eldr nodes), a USD Render ROP per node (Eldr's run $ELDR_HUSK: deferred
  skinning for crowds; Karma's plain husk) and a sticky note from README.md;
- adds the "Houdini" paragraph to README.md.

Nothing switches automatically: pick a node, render its ROP.
houdini/make_houdini_tests.py builds the houdini-tests/ scenes with the same
helpers.
"""
import json
import os
import re
import sys

import hou
from pxr import Usd, UsdRender

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELDR = "HdVkRendererPlugin"
KARMA = "BRAY_HdKarma"

# Every setting the eldr_final node ticks, one by one: Eldr's final quality is
# no mode, just these settings (lane 65's list). Edit here and rerun.
# tests/scenes/final_settings.usda's /Render/final, then the production extras.
FINAL = [
    ("eldr:filmRestart", "bool", True),
    ("eldr:rtReflections", "bool", True),
    ("eldr:rtRefraction", "bool", True),
    ("eldr:rtHitShadows", "bool", True),
    ("eldr:finalGather", "bool", True),
    ("eldr:caustics", "bool", True),
    ("eldr:rtShadowSamples", "int", 1),
    ("eldr:rtShadowLights", "int", 16),
    ("eldr:gi", "bool", True),
    ("eldr:rtShadows", "bool", True),
    ("eldr:denoise", "bool", True),
    ("eldr:filmSamples", "int", 256),
]
OIDN = '["denoise",{"engine":"oidn","use_n_input":true,"use_albedo_input":true}]'
# Karma's twin of an Eldr render setting (same meaning), for the karma prim and node.
KARMA_TWINS = {
    "eldr:filmSamples": "karma:global:pathtracedsamples",
    "eldr:colorLimit": "karma:global:colorlimit",
    "eldr:lightQuality": "karma:global:light_sampling_quality",
}
STANDARD = ("eldr_default", "eldr_final", "karma")
BEGIN = "    # --- standard settings (houdini/make_showcase_hip.py; rewritten on every run) ---\n"
END = "    # --- end standard settings ---\n"


def usda_value(type_name, value):
    if type_name == "bool":
        return "1" if value else "0"
    if type_name in ("string", "token"):
        return json.dumps(value)
    if isinstance(value, (tuple, list)) or hasattr(value, "__len__"):
        return "(%s)" % ", ".join(repr(v) for v in value)
    return repr(value)


def karma_attrs(own):
    """Karma's settings matching Eldr's final ones plus the feature's own
    (`own`: the scene's main settings prim's attributes)."""
    out = {"karma:global:pathtracedsamples": ("int", dict((a, v) for a, _, v in FINAL)["eldr:filmSamples"])}
    for name, (t, v) in own.items():
        if name.startswith("karma:"):
            out[name] = (t, v)
        elif name in KARMA_TWINS:
            # Eldr's colour limit 0 is off; Karma's 0 renders black, so off is a huge limit there.
            out[KARMA_TWINS[name]] = (t, 1e6 if name == "eldr:colorLimit" and v == 0 else v)
    if any(a == "eldr:denoise" and v for a, _, v in FINAL) and "karma:global:imagefilter" not in out:
        out["karma:global:imagefilter"] = ("string", OIDN)
    return out


def read_scene(usda):
    """Camera, resolution, products and the settings prims of a scene, the
    main one (renderSettingsPrimPath, else the first) first."""
    stage = Usd.Stage.Open(usda)
    prims = [p for p in stage.Traverse() if p.IsA(UsdRender.Settings) and p.GetName() not in STANDARD]
    main = stage.GetMetadata("renderSettingsPrimPath")
    prims.sort(key=lambda p: str(p.GetPath()) != main)

    def info(p):
        s = UsdRender.Settings(p)
        attrs = {a.GetName(): (str(a.GetTypeName()), a.Get()) for a in p.GetAuthoredAttributes()
                 if a.GetName().startswith(("eldr:", "karma:")) and a.Get() is not None}
        return {"name": p.GetName(), "path": str(p.GetPath()),
                "camera": [str(t) for t in s.GetCameraRel().GetTargets()],
                "resolution": tuple(s.GetResolutionAttr().Get() or (480, 270)),
                "products": [str(t) for t in s.GetProductsRel().GetTargets()], "attrs": attrs}

    return [info(p) for p in prims]


def standard_prims(main):
    """The standard prims (name, renderer, attrs, products) built from the scene's main prim."""
    final = {a: (t, v) for a, t, v in FINAL}
    products = main.get("products", [])
    return [
        {"name": "eldr_default", "renderer": ELDR, "attrs": {}, "products": products},
        {"name": "eldr_final", "renderer": ELDR, "attrs": final, "products": products},
        {"name": "karma", "renderer": KARMA, "attrs": karma_attrs(main["attrs"]), "products": products},
    ]


def write_standard_prims(usda, main):
    text = open(usda, encoding="utf-8").read()
    text = re.sub(re.escape(BEGIN) + ".*?" + re.escape(END), "", text, flags=re.S)
    clash = re.search(r'RenderSettings "(%s)"' % "|".join(STANDARD), text)
    if clash:
        raise RuntimeError("%s: rename its own %s prim, the standard one takes the name" % (usda, clash.group(1)))
    lines = [BEGIN]
    for p in standard_prims(main):
        lines.append('    def RenderSettings "%s"\n    {\n' % p["name"])
        if main["camera"]:
            lines.append("        rel camera = <%s>\n" % main["camera"][0])
        lines.append("        int2 resolution = (%d, %d)\n" % main["resolution"])
        if main["products"]:
            lines.append("        rel products = [%s]\n" % ", ".join("<%s>" % t for t in main["products"]))
        for name, (t, v) in sorted(p["attrs"].items()):
            lines.append("        custom %s %s = %s\n" % (t, name, usda_value(t, v)))
        lines.append("    }\n")
    lines.append(END)
    m = re.search(r'def Scope "Render"\s*\{\n', text)
    if not m:
        raise RuntimeError("%s: no Render scope" % usda)
    text = text[:m.end()] + "".join(lines) + text[m.end():]
    open(usda, "w", encoding="utf-8", newline="\n").write(text)


# --- Houdini nodes ---------------------------------------------------------


def set_parm(node, name, value, warn=True):
    """Sets a parm (or tuple) and its "<name>_control" to Set, as Solaris' attribute parms want."""
    p = node.parmTuple(name)
    if p is None and node.parm(name) is not None:  # one component, e.g. rx of r
        node.parm(name).set(value)
        return True
    if p is None:
        if warn:
            print("  %s: no parm %s" % (node.name(), name))
        return False
    if isinstance(value, bool):
        value = int(value)
    for c in p:  # e.g. the height, locked to an expression of the width in autoheight mode
        if c.isLocked():
            c.lock(False)
        if c.keyframes():
            c.deleteAllKeyframes()
    p.set(tuple(value) if len(p) > 1 else (value,))
    control = name + "_control"
    if name.startswith("xn__"):  # encoded attribute parm: its control is encode(<attribute>_control)
        control = hou.text.encodeParm(hou.text.decodeParm(name) + "_control")
    c = node.parm(control)
    if c is not None:
        c.set("set")
    SET.setdefault(node.path(), set()).update((name, control))
    return True


SET = {}  # node path -> parm names set_parm set (slim keeps these)


def set_attr(node, attr, value, warn=True):
    return set_parm(node, hou.text.encodeParm(attr), value, warn)


def set_karma(node, attr, value):
    """A karma: attribute on the Karma Render Settings LOP: its parm is the
    attribute's last name (colorlimit, diffusequality...); the OIDN image
    filter is its Denoiser menu."""
    if attr == "karma:global:imagefilter":
        return set_parm(node, "denoiser", "oidn" if "oidn" in str(value) else "off")
    return set_parm(node, attr.split(":")[-1], value, warn=False) or set_attr(node, attr, value)


def settings_node(net, inp, prim, camera, res):
    """A Render Settings LOP (Eldr's tab) or Karma Render Settings LOP for one prim."""
    path = "/Render/" + prim["name"]
    camera = (prim.get("camera") or [camera])[0]
    res = prim.get("resolution") or res
    karma = prim.get("node", "karma" if prim["renderer"] == KARMA else "eldr") == "karma"
    n = net.createNode("karmarendersettings" if karma else "rendersettings", prim["name"])
    n.setInput(0, inp)
    set_parm(n, "res_mode", "manual")
    set_parm(n, "primpath", path)
    set_parm(n, "camera", camera)
    if karma:
        set_parm(n, "resolution", res)
        # ponytail: Karma's node makes its own beauty product; the scene's AOV products stay on the Eldr nodes.
        set_parm(n, "productName", "$HIP/render/%s.$F4.exr" % prim["name"], warn=False)
        for attr, (_, v) in sorted(prim["attrs"].items()):
            set_karma(n, attr, v)
    else:
        set_parm(n, "resolution", res)
        if prim.get("products"):
            set_parm(n, "products", " ".join(prim["products"]))
        for attr, (_, v) in sorted(prim["attrs"].items()):
            set_attr(n, attr, v)
        slim(n, full=prim["name"] == "eldr_default")
    return n


def slim(node, full):
    """Every Render Settings LOP saves all renderers' tabs as spare parms
    (~440 KB per node). Keep Eldr's tab only: whole on eldr_default (to tick more),
    on the others just the settings the node sets."""
    g = node.parmTemplateGroup()
    for folder in ("Karma", "Husk"):
        f = g.findFolder(folder)
        if f:
            g.remove(f)
    if not full:
        eldr = g.findFolder("Eldr")
        keep = SET.get(node.path(), set())

        def prune(folder):
            for t in folder.parmTemplates():
                if t.type() == hou.parmTemplateType.Folder:
                    prune(t)
                elif t.name() not in keep:
                    g.remove(t.name())
        if eldr:
            prune(eldr)
    node.setParmTemplateGroup(g)


def rop_node(net, inp, prim, out_dir="$HIP/render"):
    r = net.createNode("usdrender_rop", "render_" + prim["name"])
    r.setInput(0, inp)
    set_parm(r, "rendersettings", "/Render/" + prim["name"])
    set_parm(r, "renderer", prim["renderer"])
    if prim["renderer"] == ELDR:  # husk with deferred skinning (crowds); Karma can't skin deferred
        set_parm(r, "rendercommand", "$ELDR_HUSK")
    if prim.get("timelimit"):
        set_parm(r, "husk_dotimelimit", 1)
        set_parm(r, "husk_timelimit", prim["timelimit"])
    set_parm(r, "outputimage", "%s/%s.$F4.exr" % (out_dir, prim["name"]))
    return r



def sticky(net, text):
    s = net.createStickyNote("what_to_look_at")
    s.setText(text)
    s.setSize(hou.Vector2(9, 6))
    s.setPosition(hou.Vector2(-12, 0))
    return s


def add_render_nodes(net, inp, camera, res, prims, note=""):
    """Chains one settings node per prim after `inp`, ends in an OUT null with
    the display flag, hangs a ROP per prim off it."""
    SET.clear()
    last = inp
    for p in prims:
        last = settings_node(net, last, p, camera, res)
    out = net.createNode("null", "OUT")
    out.setInput(0, last)
    out.setDisplayFlag(True)
    for p in prims:
        rop_node(net, out, p)
    if note:
        sticky(net, note)
    net.layoutChildren()
    return out


def readme_note(readme):
    """README.md up to its "Open" section, for the sticky note."""
    text = open(readme, encoding="utf-8").read() if os.path.exists(readme) else ""
    text = re.split(r"\n## (Open|Houdini)", text)[0].strip()
    return text[:2500]


HOUDINI_README = """
## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
"""


def build_showcase(folder):
    folder = os.path.abspath(folder)
    usda = os.path.join(folder, "scene.usda")
    prims = read_scene(usda)
    main = prims[0]
    write_standard_prims(usda, main)
    std = standard_prims(main)
    ab = [dict(p, renderer=ELDR) for p in prims]
    camera = main["camera"][0] if main["camera"] else "/cam"

    hou.hipFile.clear(suppress_save_prompt=True)
    net = hou.node("/stage")
    sub = net.createNode("sublayer", "scene")
    sub.parm("filepath1").set("$HIP/scene.usda")
    readme = os.path.join(folder, "README.md")
    add_render_nodes(net, sub, camera, main["resolution"], std + ab, readme_note(readme))
    hou.hipFile.save(os.path.join(folder, "scene.hip"))
    if os.path.exists(readme):
        text = open(readme, encoding="utf-8").read().split("\n## Houdini scene")[0].rstrip("\n") + "\n"
        open(readme, "w", encoding="utf-8", newline="\n").write(text + HOUDINI_README)
    print("built", os.path.join(folder, "scene.hip"), [p["name"] for p in std + ab])


if __name__ == "__main__":
    folders = sys.argv[1:] or sorted(
        os.path.join(REPO, "showcase", d) for d in os.listdir(os.path.join(REPO, "showcase"))
        if os.path.exists(os.path.join(REPO, "showcase", d, "scene.usda")))
    for f in folders:
        build_showcase(f)
