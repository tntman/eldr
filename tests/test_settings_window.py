"""Headless check of usdview's Eldr > Render Settings window
(usdview/hdvkUsdview/settings.py) against the real renderer's settings:
every setting has a group, the editors set them, the filter and reset work.
Run: usdenv.cmd python tests\\test_settings_window.py"""
import sys
from types import SimpleNamespace

from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from pxr import UsdImagingGL
from pxr.Usdviewq.qt import QtWidgets

from hdvkUsdview.settings import GROUPS, Groups, _SettingsWindow

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


app = QtWidgets.QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()
glContext.create()
glContext.makeCurrent(surface)
engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
settings = [s for s in engine.GetRendererSettingsList() if str(s.key).startswith("eldr:")]

groups = Groups(settings)
other = [str(s.key) for title, g in groups if title == "Other" for s in g]
expect(not other, "every setting has a group (ungrouped: %s)" % other)
expect(sum(len(g) for _, g in groups) == len(settings), "every setting shown once")
listed = [k for _, keys in GROUPS for k in keys.split()]
expect(len(listed) == len(set(listed)), "no setting in two groups")

window = _SettingsWindow(SimpleNamespace(qMainWindow=None), engine)
window.Rebuild()
rows = {str(s.key): (s, label, editor) for s, label, editor, _ in window._rows}
expect(len(rows) == len(settings), "a row per setting")
_, label, box = rows["eldr:ssr"]
box.setChecked(False)
expect(engine.GetRendererSetting("eldr:ssr") is False and label.font().bold(), "a checkbox sets a flag, marked changed")
rows["eldr:shadowResolution"][2].setValue(1024)
expect(engine.GetRendererSetting("eldr:shadowResolution") == 1024, "a spin box sets an int")
window._filter.setText("fog")
expect(rows["eldr:ssr"][2].isHidden() and not rows["eldr:fogDensity"][2].isHidden(), "filter")
window._filter.setText("")
window._ResetAll()
expect(engine.GetRendererSetting("eldr:ssr") is True and engine.GetRendererSetting("eldr:shadowResolution") == 2048,
       "reset to defaults")

del engine
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
