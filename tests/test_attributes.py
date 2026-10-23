"""Headless check of usdview's Eldr > Attribute Editor (usdview/hdvkUsdview/
attributes.py): values shown as text and read back, edits in the session
layer, and the window's editors.
Run: usdenv.cmd python tests\\test_attributes.py"""
import os
import sys
from types import SimpleNamespace

os.environ["QT_QPA_PLATFORM"] = "offscreen"

from pxr import Gf, Sdf, Usd, UsdLux
from pxr.Usdviewq.qt import QtCore, QtWidgets

from hdvkUsdview.attributes import Set, Text, Value, _AttributeWindow

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


app = QtWidgets.QApplication(sys.argv)
stage = Usd.Stage.CreateInMemory()
light = UsdLux.DistantLight.Define(stage, "/sun")
prim = light.GetPrim()

# Each type's value shown as text reads back as the same value.
T = Sdf.ValueTypeNames
for typeName, value in [(T.Float, 0.1), (T.Double, 1e-3), (T.Int, 4), (T.Color3f, Gf.Vec3f(1, 0.5, 0)),
                        (T.Matrix4d, Gf.Matrix4d(2).SetTranslateOnly(Gf.Vec3d(1, 2, 3))),
                        (T.Quatf, Gf.Quatf(0.5, Gf.Vec3f(0.5, 0.5, 0.5))), (T.Float3Array, [(1, 2, 3), (4, 5, 6)]),
                        (T.TokenArray, ["a", 'b "c"']), (T.Token, "a b"), (T.String, "x"), (T.Asset, Sdf.AssetPath("t.png"))]:
    attr = prim.CreateAttribute("test:" + str(typeName).replace("[]", "Array"), typeName)
    attr.Set(value)
    text = Text(attr.Get(), typeName)
    try:
        expect(Value(text, typeName) == attr.Get(), "%s reads back from %r" % (typeName, text))
    except ValueError as e:
        expect(False, "%s: %s" % (typeName, e))
expect(Text(prim.GetAttribute("test:float").Get(), T.Float) == "0.1", "float32 shown short")
expect(Text(list(range(100)), T.IntArray) == "[100 values]", "long arrays shown as their length")
for typeName, text in [(T.Color3f, "(1, 2)"), (T.Float, "abc"), (T.Float, "1\n}\ndef X {")]:
    try:
        Value(text, typeName)
        expect(False, "%r isn't a %s" % (text, typeName))
    except ValueError:
        pass

# Edits go to the session layer; removing them restores the file's value,
# and a default there wins over the file's animation.
intensity = light.GetIntensityAttr()
intensity.Set(3.0)
intensity.Set(7.0, 10)
Set(intensity, 5.0)
expect(intensity.Get(10) == 5.0 and stage.GetRootLayer().GetAttributeAtPath("/sun.inputs:intensity").default == 3.0,
       "edit in the session layer, over the file's animation")
Set(intensity, None)
expect(intensity.Get(10) == 7.0 and not stage.GetSessionLayer().GetAttributeAtPath("/sun.inputs:intensity"),
       "revert")


# The window, on a stand-in for usdview's data model.
class Selection(QtCore.QObject):
    signalPrimSelectionChanged = QtCore.Signal(set, set)

    def getFocusPrim(self):
        return prim


class Model(QtCore.QObject):
    currentFrameChanged = QtCore.Signal(object)
    signalStageReplaced = QtCore.Signal()
    signalPrimsChanged = QtCore.Signal(object, object)
    currentFrame = Usd.TimeCode(10)
    selection = Selection()


window = _AttributeWindow(SimpleNamespace(qMainWindow=None, dataModel=Model()))
window.show()
window.Rebuild()
table = window._table
row = {table.item(r, 0).text(): r for r in range(table.rowCount())}
cell = lambda name: table.item(row[name], 2)
expect(cell("inputs:intensity").text() == "7", "shows the value at the current frame")
cell("inputs:intensity").setText("2.5")
expect(intensity.Get(10) == 2.5, "text edit sets the value")
cell("inputs:color").setText("(1, 0.5, 0)")
expect(light.GetColorAttr().Get() == Gf.Vec3f(1, 0.5, 0), "color edit")
app.processEvents()
cell("inputs:color").setText("(1, 0.5)")
expect(light.GetColorAttr().Get() == Gf.Vec3f(1, 0.5, 0) and "color3f" in window._status.text(), "bad text refused")
app.processEvents()
expect(cell("inputs:color").text() == "(1, 0.5, 0)", "bad text replaced by the value")
expect(table.item(row["inputs:intensity"], 0).font().bold(), "edited attributes bold")
cell("inputs:normalize").setCheckState(QtCore.Qt.Checked)
expect(light.GetNormalizeAttr().Get() is True, "checkbox sets a bool")
combo = table.cellWidget(row["collection:shadowLink:expansionRule"], 2)
combo.setCurrentText("explicitOnly")
rule = prim.GetAttribute("collection:shadowLink:expansionRule")
expect(rule.Get() == "explicitOnly", "allowed tokens from a list")
window._filter.setText("INTENS")
expect(not table.isRowHidden(row["inputs:intensity"]) and table.isRowHidden(row["inputs:color"]), "filter")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
