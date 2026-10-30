r"""hython tests\houdini_ui.py: Eldr's tabs on the Render Settings and Render
Geometry Settings LOPs author eldr: attributes on the stage. Run with the
Houdini build's package (HOUDINI_PACKAGE_DIR=<build-h22>\\houdini) and
PXR_PLUGINPATH_NAME=<build-h22>; see docs/houdini.md."""
import sys

import hou
from pxr import Sdf, UsdGeom

stage = hou.node("/stage")


def tab(node):
    f = node.parmTemplateGroup().findFolder("Eldr")
    assert f is not None, node.type().name() + " has no Eldr tab"
    return f


def author(node, attr, value):
    node.parm(hou.text.encodeParm(attr + "_control")).set("set")
    node.parm(hou.text.encodeParm(attr)).set(value)


# Render Settings LOP: a flag, an int, a float and a menu setting.
rs = stage.createNode("rendersettings")
n = len(tab(rs).parmTemplates())
assert n >= 8, n  # the groups
last = tab(rs).parmTemplates()[-1]
assert last.label() == "Performance and Debug", last.label()
author(rs, "eldr:gi", 1)
author(rs, "eldr:filmSamples", 7)
author(rs, "eldr:bloomIntensity", 0.5)
author(rs, "eldr:tonemapper", "agx")
rs.cook(force=True)
prim = rs.stage().GetPrimAtPath(rs.parm("primpath").eval() or rs.evalParm("primpattern"))
if not prim:
    prim = next(p for p in rs.stage().Traverse() if p.GetTypeName() == "RenderSettings")
for name, expect in (("eldr:gi", True), ("eldr:filmSamples", 7), ("eldr:bloomIntensity", 0.5), ("eldr:tonemapper", "agx")):
    got = prim.GetAttribute(name).Get()
    assert got == expect and type(got) is type(expect), (name, got, expect)
assert not prim.GetAttribute("eldr:shadows"), "unset (Do Nothing) parms must not author"

# Render Geometry Settings LOP: a primvar on a mesh.
mesh = stage.createNode("cube")
rg = stage.createNode("rendergeometrysettings")
rg.setInput(0, mesh)
tab(rg)
rg.parm("primpattern").set("/cube1")
author(rg, "primvars:eldr:hair", False)
rg.cook(force=True)
cube = rg.stage().GetPrimAtPath("/cube1")
assert cube, [p.GetPath() for p in rg.stage().Traverse()]
got = cube.GetAttribute("primvars:eldr:hair").Get()
assert got is False, got
# Viewport Display Options: HdVkRendererPlugin_Viewport.ds is found through
# HOUDINI_SOHO_PATH and every parm is in the "viewport" scope, named by the
# setting's key (the viewport hands the decoded name to SetRenderSetting).
import os

ds = [d + "/parameters/HdVkRendererPlugin_Viewport.ds" for d in hou.houdiniPath("HOUDINI_SOHO_PATH")]
ds = [f for f in ds if os.path.exists(f)]
assert ds, "no HdVkRendererPlugin_Viewport.ds on HOUDINI_SOHO_PATH"
group = hou.ParmTemplateGroup()
group.setToDialogScript(open(ds[0]).read())
parms = [t for folder in group.entries() for t in folder.parmTemplates()]
assert len(parms) > 100 and all(t.tags().get("uiscope") == "viewport" for t in parms), len(parms)
assert hou.text.decodeParm(parms[0].name()).startswith("eldr:"), parms[0].name()
# Every setting once, every parm helped, important folders open and the rest folded.
names = [hou.text.decodeParm(t.name()) for t in parms]
assert len(names) == len(set(names)), "a setting appears twice"
assert all(t.help().strip() for t in parms), [t.name() for t in parms if not t.help().strip()]
folders = tab(rs).parmTemplates()  # the Render Settings LOP tab (the viewport's are plain tabs)
assert [f.label() for f in folders if f.tags().get("group_default") == "1"] == ["Sampling", "Ray Tracing", "Global Illumination", "Camera", "Image", "Volumes and Fog", "Viewport"], \
    [(f.label(), f.tags()) for f in folders]
assert folders[-1].label() == "Performance and Debug"
for t in tab(rs).parmTemplates():  # the Render Settings LOP tab
    if t.type() == hou.parmTemplateType.Folder:
        assert all(p.help().strip() for p in t.parmTemplates() if "_control" not in p.name()), t.label()
assert all(t.help().strip() for t in tab(rg).parmTemplates() if "_control" not in t.name()), "primvar help"
print("houdini_ui OK")
sys.exit(0)
