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
print("houdini_ui OK")
sys.exit(0)
