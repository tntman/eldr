"""Writes Eldr's Solaris UI definitions (Houdini dialog scripts) from the
renderer's own setting descriptors, so they cannot drift from the code.

    hython gen_ui.py <out dir> <usdview/hdvkUsdview/settings.py>

Needs PXR_PLUGINPATH_NAME to find Eldr's plugInfo.json (the build dir). Writes
<out>/soho/parameters/HdVkRendererPlugin_{Global,Geometry,Viewport}.ds, which
Houdini finds by the renderer's name through HOUDINI_SOHO_PATH:

- Global: the "Eldr" tab of the Render Settings LOP (loputils.addRendererParmFolders)
- Geometry: the "Eldr" tab of the Render Geometry Settings LOP
- Viewport: the Eldr section of the viewport's Display Options (uiscope viewport)

Attribute parms are named hou.text.encodeParm(<attribute>), each with a
"<attribute>_control" menu (Do Nothing / Set or Create / Block) as Karma's.
The groups are usdview's settings window's (GROUPS in settings.py): the
important ones open, the rest folded. Every parm's help is the "What it does"
column of docs/settings.md (usdview/hdvkUsdview/tooltips.py).
"""
import ast
import json
import os
import re
import sys

import importlib.util

import hou
from pxr import UsdImagingGL

RENDERER = "HdVkRendererPlugin"

# Per-prim primvars Eldr reads (docs/features.md, "USD authoring conventions"):
# name, label, usd type, default, help. They have no descriptors.
PRIMVARS = [
    ("primvars:eldr:reflectionProbe", "Reflection Probe", "bool", False,
     "The mesh (typically an invisible Cube) is a reflection probe: never drawn, its world box is the "
     "influence and parallax volume. Needs the Reflection Probe render setting."),
    ("primvars:eldr:planarReflection", "Planar Reflection", "bool", False,
     "A flat mesh that mirrors the scene in planar reflections. Needs the Planar Reflections render setting."),
    ("primvars:eldr:hair", "Hair Shading", "bool", True,
     "BasisCurves shade as hair (Kajiya-Kay). Off: ordinary surfaces with SSR (cables, ropes)."),
    ("primvars:eldr:ribbons", "Curves as Ribbons", "bool", False,
     "BasisCurves draw as camera-facing ribbons instead of tubes, whatever the render setting says."),
    ("primvars:eldr:sprites", "Points as Discs", "bool", False,
     "Points draw as camera-facing discs instead of spheres, whatever the render setting says."),
]


def groups(settings_py):
    """usdview's GROUPS [(title, open, "key key ...")], read without importing Qt."""
    tree = ast.parse(open(settings_py, encoding="utf-8").read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and node.targets[0].id == "GROUPS":
            return ast.literal_eval(node.value)
    raise SystemExit("no GROUPS in " + settings_py)


def literal(py, name):
    """A module-level literal of a Python file, read without importing it (Qt)."""
    for node in ast.parse(open(py, encoding="utf-8").read()).body:
        if isinstance(node, ast.Assign) and node.targets[0].id == name:
            return ast.literal_eval(node.value)
    raise SystemExit("no %s in %s" % (name, py))


SUFFIX = {"ms": " (ms)", "B": " (bytes)", "%": " (%)", "s": " (s)", "EV": " (EV)"}


def write_renderers(out, settings_py):
    """UsdRenderers.json: the template's fixed stats plus every row of usdview's
    Render Statistics window (STATS, PASSES in hdvkUsdview/__init__.py), in its
    order, as 'Group: label' entries in viewstats and statsdatapaths."""
    here = os.path.dirname(os.path.abspath(__file__))
    view = os.path.join(os.path.dirname(settings_py), "__init__.py")
    doc = json.load(open(os.path.join(here, "UsdRenderers.json"), encoding="utf-8"))
    info = doc[RENDERER]
    paths, names = info.setdefault("statsdatapaths", {}), info.setdefault("viewstats", [])
    passes = literal(view, "PASSES")
    for heading, entries in literal(view, "STATS"):
        if entries == "PASSES":
            entries = [(label, "gpuPassMs." + key, "ms") for key, label in passes]
        for label, key, unit in entries:
            name = "%s: %s%s" % (heading, label.strip(), SUFFIX.get(unit, ""))
            paths[name] = "$." + ".".join(key.split("."))
            names.append(name)
    write(f"{out}/UsdRenderers.json", json.dumps(doc, indent=4) + "\n")
    return len(names)


def q(text):
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'


def label_help(name, tip=""):
    """'Depth Prepass (off: no AO)' -> label 'Depth Prepass'; help = the docs' text."""
    return name.split(" (")[0], tip or name


def tooltips(settings_py):
    """{key: help} from docs/settings.md, through usdview's tooltips.py (loaded by path)."""
    here = os.path.dirname(os.path.abspath(settings_py))
    spec = importlib.util.spec_from_file_location("eldr_tooltips", os.path.join(here, "tooltips.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.Tooltips(os.path.join(here, "..", "..", "docs", "settings.md"))


def entry(attr, label, kind, default, help_, scope, control=True, menu=None, rng=None):
    """One attribute's parms: the control menu (unless the viewport), then the value."""
    value = hou.text.encodeParm(attr)
    ctl = hou.text.encodeParm(attr + "_control")
    ptype = {"bool": "toggle", "int": "int", "float": "float", "string": "string"}[kind]
    if kind == "bool":
        dflt = "1" if default else "0"
    elif kind == "string":
        dflt = q(default)
    else:
        dflt = repr(default)
    out = ""
    if control:
        out += f'''    parm {{
        name    "{ctl}"
        label   {q(label)}
        type    string
        default {{ "none" }}
        menujoin {{
            [ "import loputils" ]
            [ "return loputils.createEditPropertiesControlMenu(kwargs, '{kind}')" ]
            language python
        }}
        parmtag {{ "sidefx::look" "icon" }}
    }}
'''
    out += f'''    parm {{
        name    "{value}"
        label   {q(label)}
        type    {ptype}
        size    1
        parmtag {{ "uiscope" {q(scope)} }}
        parmtag {{ "usdvaluetype" "{kind}" }}
        help    {q(help_)}
        default {{ {dflt} }}
'''
    if rng:
        out += f"        range   {{ {rng[0]} {rng[1]} }}\n"
    if menu:
        out += "        menu {\n" + "".join(f"            {q(m)} {q(m)}\n" for m in menu) + "        }\n"
    if control:
        out += f'        disablewhen "{{ {ctl} == block }} {{ {ctl} == none }}"\n'
    return out + "    }\n"


def setting_entry(s, scope, control, tip=""):
    kind = {"FLAG": "bool", "INT": "int", "FLOAT": "float", "STRING": "string"}[str(s.type).split(".")[-1]]
    label, help_ = label_help(s.name, tip)
    menu = rng = None
    paren = re.search(r"\(([^()]*)\)\s*$", s.name)
    if paren and kind == "string" and "," in paren.group(1):
        menu = [m.strip() for m in paren.group(1).split(",")]
        if str(s.defValue) not in menu:
            menu = None  # not a list of values
    elif paren and kind in ("int", "float"):
        m = re.fullmatch(r"(-?[\d.]+)\s*-\s*(-?[\d.]+)", paren.group(1).strip())
        if m:
            rng = (m.group(1), m.group(2))  # soft range, not locked
    default = s.defValue if kind != "bool" else bool(s.defValue)
    default = {"int": int, "float": float}.get(kind, lambda v: v)(default) if kind in ("int", "float") else default
    return entry(str(s.key), label, kind, default, help_, scope, control, menu, rng)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


OPEN = '        parmtag { "group_default" "1" }\n'  # a collapsible folder starts open

HEADER = '''/// Automatically generated by houdini/gen_ui.py from Eldr's setting descriptors. Do not edit.

{
    name        "eldr"
    label       "Eldr"
    parmtag     { spare_opfilter        "!!SHOP/PROPERTIES!!" }
    parmtag     { spare_classtags       "render" }

'''


def main(out, settings_py):
    engine = UsdImagingGL.Engine()
    if not engine.SetRendererPlugin(RENDERER):
        raise SystemExit("cannot load " + RENDERER + " (PXR_PLUGINPATH_NAME?)")
    by_key = {str(s.key): s for s in engine.GetRendererSettingsList() if str(s.key).startswith("eldr:")}
    tips = tooltips(settings_py)
    grouped = []  # (title, [setting], open)
    seen = set()
    for title, isOpen, keys in groups(settings_py):
        found = [by_key["eldr:" + k] for k in keys.split() if "eldr:" + k in by_key]
        seen.update(str(s.key) for s in found)
        grouped.append((title, found, isOpen))
    grouped.append(("Other", [s for k, s in by_key.items() if k not in seen], False))
    grouped = [g for g in grouped if g[1]]

    def folder(title, i, body, isOpen, scope=None):
        tag = '        parmtag { "uiscope" "viewport" }\n' if scope == "viewport" else ""
        tag += OPEN if isOpen else ""
        return (f'    {"group" if scope else "groupcollapsible"} {{\n        name {q("eldr_g%d" % i)}\n'
                f'        label {q(title)}\n{tag}{body}    }}\n')

    for kind, scope, control in (("Global", "None", True), ("Viewport", "viewport", False)):
        text = HEADER
        for i, (title, group, isOpen) in enumerate(grouped):
            text += folder(title, i, "".join(setting_entry(s, scope, control, tips.get(str(s.key), "")) for s in group),
                           isOpen, "viewport" if kind == "Viewport" else None)
        write(f"{out}/soho/parameters/{hou.text.alphaNumeric(RENDERER)}_{kind}.ds", text + "}\n")

    text = HEADER
    for p in PRIMVARS:
        text += entry(p[0], p[1], p[2], p[3], p[4], "None")
    write(f"{out}/soho/parameters/{hou.text.alphaNumeric(RENDERER)}_Geometry.ds", text + "}\n")
    stats = write_renderers(out, settings_py)
    print(f"Eldr UI: {len(by_key)} settings in {len(grouped)} groups, {len(PRIMVARS)} primvars, {stats} stats -> {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
