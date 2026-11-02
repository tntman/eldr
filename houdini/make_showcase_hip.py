r"""Builds showcase/<folder>/scene.hip for every showcase (or the folders given):

    hython houdini/make_showcase_hip.py [showcase/<folder> ...]

Run with Eldr's Houdini package (HOUDINI_PACKAGE_DIR=<build-h22>\houdini,
PXR_PLUGINPATH_NAME=<build-h22>) so the Eldr tab of the Render Settings LOP
exists. For each folder it

- splits scene.usda (flattened) into geo.usda, the geometry only (meshes,
  curves, points, instancers, volumes, skeletons, xforms, with their
  animation and material bindings), and scene.usda, which sublayers
  geo.usda and keeps the cameras, lights, materials and render prims;
- writes the standard RenderSettings prims into scene.usda (between the
  "standard settings" markers, rewritten every run): eldr_default (Eldr's
  defaults), eldr_final (every setting of FINAL ticked) and karma (Karma to
  match: same camera, resolution, samples, products, Karma's twins);
- builds scene.hip: a Sublayer LOP on $HIP/geo.usda; a Houdini node per
  camera, light, render product and render var (the node Solaris itself
  opens to edit that prim type, its parms set from the prim; properties
  without a parm become spare parms); a Material Library holding every
  material as VOPs (imported by Solaris' Edit Material Network, then
  repaired: signatures, ramps, dropped nodes), an Edit Properties LOP for a
  value the VOPs can't carry, a Reference LOP from scene.usda for a material
  they can't rebuild; one render settings node per settings prim (the
  standard three, then the scene's own A/B prims as Eldr nodes), a USD
  Render ROP per node (Eldr's run $ELDR_HUSK: deferred skinning for crowds;
  Karma's plain husk) and a sticky note from README.md;
- adds the "Houdini" paragraph to README.md.

Nothing switches automatically: pick a node, render its ROP. With --check
it compares the hip's stage with scene.usda prim by prim (values, wires,
transforms at every time sample) and exits 1 on a difference.
houdini/make_houdini_tests.py builds the houdini-tests/ scenes with the same
helpers.
"""
import json
import os
import re
import sys

import hou
import loputils
from pxr import Sdf, Sdr, Usd, UsdGeom, UsdLux, UsdRender, UsdShade

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
DEFAULT_PRIM = "/Render/eldr_default"  # the viewport's "Default" render settings
STANDARD = ("eldr_default", "eldr_final", "karma")
BEGIN = "    # --- standard settings (houdini/make_showcase_hip.py; rewritten on every run) ---\n"
MAIN = "    # main settings prim: "
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
    marked = re.search(re.escape(MAIN) + r"(\S+)", open(usda, encoding="utf-8").read())
    if marked:  # a rerun: the metadata points at eldr_default now
        main = marked.group(1)
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
    lines = [BEGIN, "%s%s\n" % (MAIN, main["path"])]
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
    meta = 'renderSettingsPrimPath = "%s"' % DEFAULT_PRIM
    if "renderSettingsPrimPath = " in text:
        text = re.sub(r"renderSettingsPrimPath = .*", lambda _: meta, text, count=1)
    elif text.startswith("#usda 1.0\n(\n"):
        text = text.replace("#usda 1.0\n(\n", "#usda 1.0\n(\n    " + meta + "\n", 1)
    else:
        text = text.replace("#usda 1.0\n", "#usda 1.0\n(\n    " + meta + "\n)\n", 1)
    open(usda, "w", encoding="utf-8", newline="\n").write(text)


# --- geo.usda split ----------------------------------------------------------
# Prim types the hip builds from Houdini nodes; everything else (meshes,
# curves, points, instancers, volumes, skeletons, xforms) is geometry.
NODE_TYPES = ("Camera", "Material", "NodeGraph", "RenderSettings", "RenderProduct", "RenderVar", "RenderPass")


def is_node_prim(prim):
    return prim.GetTypeName() in NODE_TYPES or (prim.HasAPI(UsdLux.LightAPI) and not prim.IsA(UsdGeom.Gprim))


def node_prims(stage):
    """Top-most prims the hip makes with nodes, in stage order."""
    out = []
    it = iter(Usd.PrimRange(stage.GetPseudoRoot()))
    for p in it:
        if is_node_prim(p):
            out.append(p)
            it.PruneChildren()
    return out


def _relative_assets(layer, folder):
    """Flattening anchors asset paths; make the ones inside the repo relative to the showcase folder again."""
    def fix(v):
        if isinstance(v, Sdf.AssetPath):
            p = v.path.replace("\\", "/")
            if os.path.isabs(p) and os.path.normcase(os.path.abspath(p)).startswith(os.path.normcase(REPO)):
                p = os.path.relpath(p, folder).replace("\\", "/")
                return Sdf.AssetPath(p if p.startswith(".") else "./" + p)
            return v
        if isinstance(v, Sdf.AssetPathArray):
            return Sdf.AssetPathArray([fix(x) for x in v])
        return v

    def visit(path):
        if not path.IsPropertyPath():
            return
        spec = layer.GetAttributeAtPath(path)
        if spec is None or spec.typeName not in (Sdf.ValueTypeNames.Asset, Sdf.ValueTypeNames.AssetArray):
            return
        if spec.HasDefaultValue():
            spec.default = fix(spec.default)
        for t in layer.ListTimeSamplesForPath(path):
            layer.SetTimeSample(path, t, fix(layer.QueryTimeSample(path, t)))
    layer.Traverse(Sdf.Path.absoluteRootPath, visit)


def _ancestors(path):
    out = []
    while path.GetParentPath() != Sdf.Path.absoluteRootPath:
        path = path.GetParentPath()
        out.append(path)
    return out


def _remove(layer, path):
    parent = layer.GetPrimAtPath(path.GetParentPath()) if path.GetParentPath() != Sdf.Path.absoluteRootPath else layer.pseudoRoot
    del parent.nameChildren[path.name]


def split_scene(folder):
    """Splits scene.usda into geo.usda (geometry only, flattened) and scene.usda
    (sublayers geo.usda; holds the cameras, lights, materials and render prims
    the hip makes with nodes). Idempotent; the standard settings prims are
    dropped here and rewritten by write_standard_prims."""
    usda = os.path.join(folder, "scene.usda")
    text = open(usda, encoding="utf-8").read()
    body = re.sub(r"^#usda 1.0\n(\(\n.*?\n\)\n)?", "", text, count=1, flags=re.S)
    m = re.match(r"\s*((?:#.*\n|\s*\n)*)", body)  # the comment block before the first prim
    comments = re.sub(r"\n{3,}", "\n\n", m.group(1)).strip("\n") if m else ""
    stage = Usd.Stage.Open(usda)
    flat = stage.Flatten()
    flat.documentation = ""  # "Generated from ... <absolute path>"
    del stage
    nodes = [p.GetPath() for p in node_prims(Usd.Stage.Open(flat))]
    _relative_assets(flat, folder)

    geo = Sdf.Layer.CreateAnonymous(".usda")
    geo.TransferContent(flat)
    for path in nodes:
        _remove(geo, path)
    # scopes left empty by the move (/Render, /materials) go too
    for path in sorted({a for p in nodes for a in _ancestors(p)}, key=lambda a: -a.pathElementCount):
        spec = geo.GetPrimAtPath(path)
        if spec and spec.typeName in ("Scope", "") and not spec.nameChildren and not spec.properties:
            _remove(geo, path)
    if geo.defaultPrim and not geo.GetPrimAtPath("/" + geo.defaultPrim):
        geo.ClearDefaultPrim()
    if geo.pseudoRoot.HasInfo("renderSettingsPrimPath"):
        geo.pseudoRoot.ClearInfo("renderSettingsPrimPath")

    rest = Sdf.Layer.CreateAnonymous(".usda")
    rest.TransferContent(flat)
    keep = set(nodes)
    ancestors = {a for p in nodes for a in _ancestors(p)}

    def prune(spec):
        for child in list(spec.nameChildren):
            path = child.path
            if path in keep:
                continue
            if path not in ancestors:
                del spec.nameChildren[child.name]
                continue
            if geo.GetPrimAtPath(path):  # geometry owns it: only an over to hang the children on
                child.specifier = Sdf.SpecifierOver
                child.typeName = ""
                for prop in list(child.properties):
                    child.RemoveProperty(prop)
                for key in ("apiSchemas", "kind", "instanceable"):
                    if child.HasInfo(key):
                        child.ClearInfo(key)
            prune(child)
    prune(rest.pseudoRoot)
    render = rest.GetPrimAtPath("/Render")
    for name in STANDARD:  # rewritten by write_standard_prims
        if render and name in render.nameChildren:
            del render.nameChildren[name]
    rest.subLayerPaths = ["./geo.usda"]

    geo_path = os.path.join(folder, "geo.usda")
    geo.Export(geo_path)
    out = rest.ExportToString()
    if comments:
        head, sep, tail = out.partition("\n)\n")
        out = head + sep + "\n" + comments + "\n" + tail
    open(usda, "w", encoding="utf-8", newline="\n").write(out)
    for p in (geo_path, usda):
        layer = Sdf.Layer.Find(p)
        if layer:
            layer.Reload(force=True)


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
    return True


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
    return n


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


# --- USD prims -> LOP nodes --------------------------------------------------
# The node Solaris itself opens to edit a prim of each type (loputils' table:
# lights, camera), the render prims' own nodes; anything else gets an Edit
# Properties LOP.
PRIM_NODE = {"RenderVar": "rendervar::2.0", "RenderProduct": "renderproduct", "RenderPass": "renderpass"}


def _authored(prim, name):
    if not Sdf.Path.IsValidNamespacedIdentifier(name):
        return False
    if name.startswith("collection:") and not prim.HasProperty(name):  # a collection parm stands for its properties
        return any(loputils._hasProperty(prim, a, True) for a in prim.GetPropertyNames() if a.startswith(name + ":"))
    return loputils._hasProperty(prim, name, True)


def prim_node(net, inp, src, prim):
    """A LOP node that creates `prim` (read from the file `src`) with its
    authored properties set: Solaris' own editor node for the prim type,
    controls "Set" for authored properties, "Do nothing" for the rest
    (USD fallbacks). Properties the node has no parm for become spare
    parms on the node, the same way Solaris' "Edit Properties" adds them;
    time-sampled values become linear keys."""
    path = str(prim.GetPath())
    t = prim.GetTypeName()
    kind = loputils.theUsdTypeToNodeTypeMap.get(t) or PRIM_NODE.get(t) or "editproperties"
    n = net.createNode(kind, hou.text.variableName(prim.GetName()))
    n.setInput(0, inp)
    n.parm("createprims").set("on")
    n.parm("primpath").set(path)
    if kind == "editproperties":
        n.parm("primtype").set(t)
    loputils.setAllControlParameters(n, "none")
    if n.parm("res_mode") is not None:
        n.parm("res_mode").set("manual")
    for toggle, prefix in (("spotlightenable", "inputs:shaping:cone:"), ("iesenable", "inputs:shaping:ies:"),
                           ("focusenable", "inputs:shaping:focus")):
        if n.parm(toggle) is not None and any(a.startswith(prefix) for a in prim.GetPropertyNames() if _authored(prim, a)):
            n.parm(toggle).set(1)
    # transforms: let the node take over the prim's local matrix
    for pt in n.parmTuples():
        if pt.parmTemplate().tags().get("usdvaluetype") == "xform" and pt.parmTemplate().dataType() == hou.parmData.String:
            pt.set(("overwriteorappend",))
    covered = {"xformOpOrder"} | {a for a in prim.GetPropertyNames() if a.startswith("xformOp:")}
    times = sorted({s for a in prim.GetAuthoredAttributes() for s in a.GetTimeSamples()})

    def init():
        loputils.initializeParametersFromPrim(n, path, src, do_nothing_parms_only=False, authored_values_only=True)

    def usd_parms():
        for pt in n.parmTuples():
            tags = pt.parmTemplate().tags()
            if "usdvaluetype" in tags:
                decoded = hou.text.decodeParm(pt.name())
                ctl = [n.parm(c) for c in (tags.get("usdcontrolparm"), hou.text.encodeParm(decoded + "_control"),
                                           decoded.replace(":", "_") + "_control") if c and n.parm(c) is not None]
                yield pt, tags, tags.get("usdvaluename") or decoded, ctl[0] if ctl else None

    for pt, tags, name, ctl in usd_parms():
        if tags["usdvaluetype"] != "xform" and _authored(prim, name):
            pt.deleteAllKeyframes()  # e.g. a resolution height that follows the camera's aspect
    init()
    xform_authored = prim.GetAttribute("xformOpOrder").HasAuthoredValue()
    for pt, tags, name, ctl in usd_parms():
        if tags["usdvaluetype"] == "xform":
            if xform_authored and ctl is not None:
                ctl.set("set")
        elif tags["usdvaluetype"] == "collection":
            # a collection parm is a prim pattern: the includes; excludes and the rest go to spare parms
            rel = prim.GetRelationship(name + ":includes")
            if rel and rel.HasAuthoredTargets():
                pt.set((" ".join(str(x) for x in rel.GetTargets()),))
                if ctl is not None:
                    ctl.set("set")
                covered.add(name + ":includes")
        elif _authored(prim, name):
            if ctl is not None:
                ctl.set("set")
            covered.add(name)
            if name.startswith("collection:"):
                covered.update(a for a in prim.GetPropertyNames() if a.startswith(name + ":"))
    rest = [a for a in prim.GetPropertyNames() if _authored(prim, a) and a not in covered
            and not any(a.startswith(c + ":") for c in covered if c.startswith("collection:"))]
    if rest:
        grp = n.parmTemplateGroup()
        for a in rest:
            loputils._addControlParmsToGroup(grp, hou.lop.createParmsForProperty(src, path, a), "set")
        n.setParmTemplateGroup(grp)
        loputils.initializeParametersFromPrim(n, path, src, selectparms={q.tuple().name() for q in n.spareParms()},
                                              do_nothing_parms_only=False, authored_values_only=True)
    # parms whose Houdini default is an expression (a resolution height following the camera's aspect)
    if n.parmTuple("resolution") is not None and prim.HasAttribute("resolution") and prim.GetAttribute("resolution").HasAuthoredValue():
        set_parm(n, "resolution", tuple(prim.GetAttribute("resolution").Get()))
    if len(times) > 1:  # animated: key every parm that changes between the samples, author the whole range
        frame = hou.frame()
        start, end = hou.playbar.frameRange()
        hou.playbar.setFrameRange(min(start, times[0]), max(end, times[-1]))
        values = {}
        for f in times:
            hou.setFrame(f)
            init()
            for p in n.parms():
                if p.parmTemplate().type() == hou.parmTemplateType.Float:
                    values.setdefault(p.name(), []).append((f, p.eval()))
        for name, keys in values.items():
            if len({v for _, v in keys}) > 1:
                p = n.parm(name)
                for f, v in keys:
                    k = hou.Keyframe(v, hou.frameToTime(f))
                    k.setExpression("linear()")
                    p.setKeyframe(k)
        hou.setFrame(frame)
        n.parm("sample_behavior").set("multi")
        n.parmTuple("sample_f").set((times[0], times[-1], min(b - a for a, b in zip(times, times[1:]))))
    if t == "Camera" and any(_authored(prim, a) for a in ("horizontalAperture", "verticalAperture")):
        # both apertures as given; loputils' init leaves the group at "do nothing"
        n.parm("aperture").set("set")
    return n, rest


RAMP_BASIS = {"linear": hou.rampBasis.Linear, "constant": hou.rampBasis.Constant,
              "catmull-rom": hou.rampBasis.CatmullRom, "catmullrom": hou.rampBasis.CatmullRom,
              "bspline": hou.rampBasis.BSpline, "monotonecubic": hou.rampBasis.MonotoneCubic,
              "hermite": hou.rampBasis.Hermite, "bezier": hou.rampBasis.Bezier}


def _set_ramps(vop, prim):
    """Ramp parms from the shader's <basis>/<keys>/<values> inputs (the import leaves them at default)."""
    if not prim:
        return
    for pt in vop.parmTuples():
        tags = pt.parmTemplate().tags()
        if pt.parmTemplate().type() != hou.parmTemplateType.Ramp or "rampkeys_var" not in tags:
            continue
        basis, keys, values = (prim.GetAttribute("inputs:" + tags[k]) for k in ("rampbasis_var", "rampkeys_var", "rampvalues_var"))
        if not (keys and keys.HasAuthoredValue() and values and values.HasAuthoredValue()):
            continue
        k, v = list(keys.Get()), [tuple(x) if hasattr(x, "__len__") else x for x in values.Get()]
        b = [RAMP_BASIS.get(str(x).lower(), hou.rampBasis.Linear)
             for x in (basis.Get() if basis and basis.HasAuthoredValue() else ["linear"] * len(k))]
        pt[0].set(hou.Ramp(b, k, v))


def _hipify(nodes, folder):
    """Absolute file paths in string parms -> $HIP- or $HFS-relative, so the hip works from any checkout."""
    hfs = hou.text.expandString("$HFS")
    folder = os.path.abspath(folder)
    for node in nodes:
        for p in node.parms():
            if p.parmTemplate().type() != hou.parmTemplateType.String or p.keyframes():
                continue
            v = p.unexpandedString()
            if not v or v.startswith("$") or not os.path.isabs(v):
                continue
            full = os.path.normcase(os.path.abspath(v))
            if full.startswith(os.path.normcase(REPO)):
                p.set("$HIP/" + os.path.relpath(v, folder).replace(os.sep, "/"))
            elif full.startswith(os.path.normcase(os.path.abspath(hfs))):
                p.set("$HFS/" + os.path.relpath(v, hfs).replace(os.sep, "/"))


def _signature(vop, shader_id):
    """The VOP signature whose shader is `shader_id` (ND_multiply_color3FA -> color3FA), or None."""
    if vop.parm("signature") is None or not shader_id:
        return None
    suffix = shader_id.split("_", 2)[-1]
    for sig in ("default", suffix, suffix.replace("_", "")):
        try:
            if vop.shaderNameForSignature(sig) == shader_id:
                return sig
        except hou.OperationFailed:
            pass
    return None


def _repair_vops(mat_vop, mat, src):
    """What Edit Material Network's import leaves out: each shader's
    signature (it may pick another one), its input values for that
    signature, ramps, and shader prims it drops (an implicit conversion),
    which get their VOP and wires back."""
    created = []
    for prim in Usd.PrimRange(mat):
        if prim == mat or prim.GetTypeName() != "Shader":
            continue
        shader_id = UsdShade.Shader(prim).GetShaderId()
        rel = str(prim.GetPath())[len(str(mat.GetPath())) + 1:]
        vop = mat_vop.node(rel)
        if vop is None:
            parent = mat_vop.node(rel.rpartition("/")[0]) if "/" in rel else mat_vop
            kind = hou.lop.shaderNodeType(shader_id or "")
            if parent is None or kind is None:
                print("  %s: no VOP for %s" % (prim.GetPath(), shader_id))
                continue
            vop = parent.createNode(kind.name(), prim.GetName())
            created.append((vop, prim))
        sig = _signature(vop, shader_id)
        if sig and vop.parm("signature").eval() != sig:
            vop.parm("signature").set(sig)
            resig = True
        else:
            resig = False
        if resig or any(v is vop for v, _ in created):
            for a in prim.GetAuthoredAttributes():
                name = a.GetName()
                if not name.startswith("inputs:") or a.HasAuthoredConnections() or not a.HasValue():
                    continue
                base = name[len("inputs:"):]
                pt = (vop.parmTuple("%s_%s" % (base, sig)) if sig and sig != "default" else None) or vop.parmTuple(base)
                if pt is not None and pt.parmTemplate().type() != hou.parmTemplateType.Ramp:
                    hou.lop.setParmTupleFromProperty(pt, src, str(prim.GetPath()), name)
        _set_ramps(vop, prim)
    # wires into and out of the VOPs made here
    made = {str(p.GetPath()) for _, p in created}
    for prim in Usd.PrimRange(mat):
        if prim.GetTypeName() != "Shader":
            continue
        for a in prim.GetAuthoredAttributes():
            for c in a.GetConnections():
                target = str(c.GetPrimPath())
                if str(prim.GetPath()) not in made and target not in made:
                    continue
                dst = mat_vop.node(str(prim.GetPath())[len(str(mat.GetPath())) + 1:])
                srcvop = mat_vop.node(target[len(str(mat.GetPath())) + 1:]) if target.startswith(str(mat.GetPath()) + "/") else None
                if dst is None or srcvop is None:
                    continue
                try:
                    dst.setNamedInput(a.GetBaseName(), srcvop, c.name.split(":", 1)[-1])
                except hou.OperationFailed as e:
                    print("  %s.%s: wire failed (%s)" % (prim.GetPath(), a.GetName(), e))


def material_node(net, inp, src, prims):
    """One Material Library LOP with every material: Solaris' Edit Material
    Network imports each USD material as VOP nodes (MaterialX, USD Preview,
    Karma), which move into the library at the same prim paths."""
    tmp_sub = net.createNode("sublayer", "tmp_src")
    tmp_sub.parm("filepath1").set(src)
    em = net.createNode("editmaterial", "tmp_import")
    em.setInput(0, tmp_sub)
    em.parm("materials").set(len(prims))
    for i, p in enumerate(prims):
        em.parm("matpath%d" % (i + 1)).set(str(p.GetPath()))
        em.parm("load%d" % (i + 1)).pressButton()
    lib = net.createNode("materiallibrary", "materials")
    lib.setInput(0, inp)
    lib.parm("genpreviewshaders").set(0)  # the scene's materials as they are, no extra preview shaders
    copied = hou.copyNodesTo(em.children(), lib)
    for c in copied:
        c.setMaterialFlag(True)
    for c in lib.allSubChildren():
        # Edit Material Network's per-parm "__activate__" toggles (author only edited values) go: in a
        # library every value that differs from the node's default is authored, as for a hand-made material.
        grp = c.parmTemplateGroup()
        names = [q.name() for q in c.spareParms() if q.name().startswith("__activate__")]
        for name in names:
            grp.remove(name)
        if names:
            c.setParmTemplateGroup(grp)
    lib.parm("materials").set(len(prims))
    by_name = {c.name(): c for c in copied}
    for i, p in enumerate(prims):
        c = by_name.get(hou.text.variableName(p.GetName())) or (copied[i] if i < len(copied) else None)
        if c is None:
            print("  material %s: not imported" % p.GetPath())
            continue
        lib.parm("matnode%d" % (i + 1)).set(c.name())
        lib.parm("matpath%d" % (i + 1)).set(str(p.GetPath()))
        _repair_vops(c, p, src)
    lib.layoutChildren()
    em.destroy()
    tmp_sub.destroy()
    return lib


def scene_nodes(net, inp, src):
    """Nodes for every camera, light, material and render prim of `src`
    except the render settings (add_render_nodes makes those). Returns the
    last node and {prim: [properties set on spare parms]}."""
    folder = os.path.dirname(os.path.abspath(src))
    stage = Usd.Stage.Open(src)
    prims = node_prims(stage)
    mats = [p for p in prims if p.GetTypeName() in ("Material", "NodeGraph")]
    last = inp
    spare = {}
    referenced = []
    if mats:
        lib = material_node(net, last, src, mats)
        out = lib.stage()
        diffs = {m: [(prim, prim_diffs(prim, out.GetPrimAtPath(prim.GetPath()))) for prim in Usd.PrimRange(m)] for m in mats}
        # A material the VOP import can't rebuild (a dropped node, another shader signature) comes in
        # whole by a Reference LOP from scene.usda instead: right, though not editable as VOPs.
        referenced = [m for m in mats if any(d[0] in ("prim", "type", "structure") for _, ds in diffs[m] for d in ds)]
        if referenced:
            lib.destroy()
            mats = [m for m in mats if m not in referenced]
            lib = material_node(net, last, src, mats) if mats else None
        if lib is not None:
            last = lib
            _hipify(lib.allSubChildren(), folder)
        # values or wires the VOPs can't carry (an input the node type lacks): an Edit Properties LOP per prim
        for m in mats:
            for prim, ds in diffs[m]:
                names = sorted({d[1] for d in ds if d[0] in ("value", "connection")})
                if names:
                    last = edit_properties(net, last, src, prim, names)
                    _hipify([last], folder)
                    spare[str(prim.GetPath())] = names
        for m in referenced:
            last = reference_node(net, last, m)
            print("  %s: referenced from scene.usda (%s)" % (m.GetPath(), "; ".join(
                d[2] for _, ds in diffs[m] for d in ds if d[0] in ("prim", "type", "structure"))[:300]))
    for p in prims:
        if p.GetTypeName() in ("Material", "NodeGraph", "RenderSettings"):
            continue
        last, rest = prim_node(net, last, src, p)
        _hipify([last], folder)
        if rest:
            spare[str(p.GetPath())] = rest
    return last, spare


def reference_node(net, inp, prim):
    """A Reference LOP bringing `prim` (and what's under it) from $HIP/scene.usda."""
    n = net.createNode("reference::2.0", "ref_" + hou.text.variableName(prim.GetName()))
    n.setInput(0, inp)
    n.parm("parentprimtype").set("UsdGeomScope")
    n.parm("primpath1").set(str(prim.GetPath()))
    n.parm("filepath1").set("$HIP/scene.usda")
    n.parm("filerefprim1").set("")  # a specific prim
    n.parm("filerefprimpath1").set(str(prim.GetPath()))
    return n


def edit_properties(net, inp, src, prim, names):
    """An Edit Properties LOP setting `names` (values or connections) on
    `prim`, its parms made as Solaris' own "Edit" does."""
    path = str(prim.GetPath())
    n = net.createNode("editproperties", "edit_" + hou.text.variableName(prim.GetName()))
    n.setInput(0, inp)
    n.parm("primpattern").set(path)
    grp = n.parmTemplateGroup()
    for a in names:
        attr = prim.GetAttribute(a)
        if attr and attr.HasAuthoredConnections():
            parms = hou.lop.createConnectionParmsForProperty(src, path, a)
        else:
            parms = hou.lop.createParmsForProperty(src, path, a)
        loputils._addControlParmsToGroup(grp, parms, "set")
    n.setParmTemplateGroup(grp)
    loputils.initializeParametersFromPrim(n, str(prim.GetPath()), src, selectparms={q.tuple().name() for q in n.spareParms()},
                                          do_nothing_parms_only=False, authored_values_only=True)
    return n


# --- check: the hip's stage against scene.usda ---------------------------------


def _same(a, b, tol=1e-4):
    if a == b:
        return True
    if isinstance(a, Sdf.AssetPath) and isinstance(b, Sdf.AssetPath):
        return os.path.normcase(os.path.abspath(a.resolvedPath or a.path)) == os.path.normcase(os.path.abspath(b.resolvedPath or b.path))
    try:
        fa = [float(x) for x in (a if hasattr(a, "__len__") and not isinstance(a, str) else [a])]
        fb = [float(x) for x in (b if hasattr(b, "__len__") and not isinstance(b, str) else [b])]
    except (TypeError, ValueError):
        try:  # matrices, nested tuples
            fa = [float(x) for row in a for x in row]
            fb = [float(x) for row in b for x in row]
        except (TypeError, ValueError):
            return str(a) == str(b)
    return len(fa) == len(fb) and all(abs(x - y) <= tol * max(1.0, abs(x)) for x, y in zip(fa, fb))


def _sdr_default(prim, name):
    shader = UsdShade.Shader(prim)
    node = shader and Sdr.Registry().GetShaderNodeByIdentifier(shader.GetShaderId() or "")
    inp = node and node.GetShaderInput(name.split(":", 1)[1])
    return inp.GetDefaultValue() if inp else None


# transforms compare as matrices; the rest is UI/bookkeeping (Houdini's material interface folders and tab menu)
SKIP_ATTRS = ("xformOp", "ui:", "houdini:", "config:mtlx:version", "inputs:tabmenumask", "inputs:folder")


def _value(attr, prim, name, s):
    return attr.Get(s) if attr and attr.HasValue() else _sdr_default(prim, name)


def prim_diffs(prim, q):
    """[(kind, property, message)] between `prim` (scene.usda) and `q` (the
    hip's stage). Values unauthored on one side compare against the schema
    fallback or the shader's default."""
    path = prim.GetPath()
    if not q:
        return [("prim", "", "missing prim %s" % path)]
    out = []
    if q.GetTypeName() != prim.GetTypeName():
        out.append(("type", "", "type %s: %s vs %s" % (path, prim.GetTypeName(), q.GetTypeName())))
    if prim.IsA(UsdGeom.Xformable):
        for s in sorted({s for a in prim.GetAuthoredAttributes() for s in a.GetTimeSamples()}) or [Usd.TimeCode.Default()]:
            if not _same(UsdGeom.Xformable(prim).GetLocalTransformation(s), UsdGeom.Xformable(q).GetLocalTransformation(s)):
                out.append(("xform", "", "xform %s @%s" % (path, s)))
    names = {a.GetName() for a in prim.GetAuthoredAttributes()} | {a.GetName() for a in q.GetAuthoredAttributes()}
    for name in sorted(names):
        if name.startswith(SKIP_ATTRS) or name == "xformOpOrder":
            continue
        a, b = prim.GetAttribute(name), q.GetAttribute(name)
        times = sorted(set(a.GetTimeSamples() if a else []) | set(b.GetTimeSamples() if b else [])) or [Usd.TimeCode.Default()]
        for s in times:
            va, vb = _value(a, prim, name, s), _value(b, q, name, s)
            if va is None and not (a and a.HasValue()):  # Houdini authors a value the scene leaves to an unknown default
                continue
            if not _same(va, vb):
                # another shader (signature) or value type: nothing an edit of the value can mend
                structural = name == "info:id" or (va is not None and vb is not None and type(va) is not type(vb))
                out.append(("structure" if structural else "value", name, "%s.%s: %r vs %r" % (path, name, va, vb)))
                break
        ca = [str(c) for c in a.GetConnections()] if a else []
        cb = [str(c) for c in b.GetConnections()] if b else []
        if ca != cb:
            out.append(("connection", name, "%s.%s connections: %s vs %s" % (path, name, ca, cb)))
    for r in prim.GetAuthoredRelationships():
        rb = q.GetRelationship(r.GetName())
        ta, tb = [str(t) for t in r.GetTargets()], [str(t) for t in (rb.GetTargets() if rb else [])]
        if ta != tb:
            out.append(("targets", r.GetName(), "%s.%s targets: %s vs %s" % (path, r.GetName(), ta, tb)))
    return out


def compare_stages(ref, out):
    """Differences between the camera/light/material/render prims of `ref`
    (scene.usda) and `out` (the hip's stage)."""
    problems = []
    for root in node_prims(ref):
        if root.GetTypeName() == "RenderSettings":  # settings_node's own territory (Karma's node authors its defaults)
            continue
        for prim in Usd.PrimRange(root):
            problems += [d[2] for d in prim_diffs(prim, out.GetPrimAtPath(prim.GetPath()))]
    return problems


def sticky(net, text):
    s = net.createStickyNote("what_to_look_at")
    s.setText(text)
    s.setSize(hou.Vector2(9, 6))
    s.setPosition(hou.Vector2(-12, 0))
    return s


def add_render_nodes(net, inp, camera, res, prims, note=""):
    """Chains one settings node per prim after `inp`, ends in an OUT null with
    the display flag, hangs a ROP per prim off it."""
    last = inp
    for p in prims:
        last = settings_node(net, last, p, camera, res)
    # each settings LOP makes its own prim the stage's default, so this goes after them all
    cfg = net.createNode("configurelayer", "default_render_settings")
    cfg.setInput(0, last)
    set_parm(cfg, "setrendersettings", 1)
    set_parm(cfg, "rendersettings", DEFAULT_PRIM)
    last = cfg
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

Open `scene.hip`. A Sublayer LOP loads `geo.usda`, the scene's geometry only. Everything else is a Houdini node you can change: the camera (Camera LOP), each light (Light, Distant Light, Dome Light LOPs), the materials (a Material Library with MaterialX, USD Preview Surface and Karma VOPs), render products and AOVs (Render Product, Render Var LOPs), and one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

A value a node has no parameter for sits on the node as an extra parameter, or on an Edit Properties LOP right after it. A material Houdini's VOPs can't rebuild exactly comes from `scene.usda` through a Reference LOP.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). `scene.usda` holds the same scene for usdview and husk (it sublayers `geo.usda`): `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda` (`--check` compares the hip's stage with `scene.usda`).
"""


def build_showcase(folder, check=False):
    folder = os.path.abspath(folder)
    usda = os.path.join(folder, "scene.usda")
    prims = read_scene(usda)
    main = prims[0]
    split_scene(folder)
    write_standard_prims(usda, main)
    std = standard_prims(main)
    ab = [dict(p, renderer=ELDR) for p in prims]
    camera = main["camera"][0] if main["camera"] else "/cam"

    hou.hipFile.clear(suppress_save_prompt=True)
    hou.hipFile.setName(os.path.join(folder, "scene.hip"))
    net = hou.node("/stage")
    sub = net.createNode("sublayer", "geometry")
    sub.parm("filepath1").set("$HIP/geo.usda")
    last, spare = scene_nodes(net, sub, usda)
    readme = os.path.join(folder, "README.md")
    out = add_render_nodes(net, last, camera, main["resolution"], std + ab, readme_note(readme))
    hou.hipFile.save(os.path.join(folder, "scene.hip"))
    if os.path.exists(readme):
        text = open(readme, encoding="utf-8").read().split("\n## Houdini scene")[0].rstrip("\n") + "\n"
        open(readme, "w", encoding="utf-8", newline="\n").write(text + HOUDINI_README)
    print("built", os.path.join(folder, "scene.hip"), [p["name"] for p in std + ab])
    for path, names in spare.items():
        print("  spare parms on %s: %s" % (path, " ".join(names)))
    if check:
        stage = out.stage()
        problems = compare_stages(Usd.Stage.Open(usda), stage) if stage else [
            "%s: %s" % (n.path(), " ".join(n.errors())) for n in net.children() if n.errors()] or ["no stage"]
        for p in problems:
            print("  DIFF", p)
        print("  check: %d differences" % len(problems))
        return len(problems)
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    check = "--check" in args
    args = [a for a in args if a != "--check"]
    folders = args or sorted(
        os.path.join(REPO, "showcase", d) for d in os.listdir(os.path.join(REPO, "showcase"))
        if os.path.exists(os.path.join(REPO, "showcase", d, "scene.usda")))
    bad = sum(build_showcase(f, check) for f in folders)
    sys.exit(1 if bad else 0)
