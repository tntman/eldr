"""Every eldr: setting the renderer exposes is documented in docs/settings.md.
Run: usdenv.cmd python tests\test_docs.py"""
import os
import re
import sys

from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from pxr import UsdImagingGL
from pxr.Usdviewq.qt import QtWidgets

app = QtWidgets.QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()
glContext.create()
glContext.makeCurrent(surface)
engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
keys = [str(s.key) for s in engine.GetRendererSettingsList() if str(s.key).startswith("eldr:")]
if os.environ.get("HDVK_DUMP_SETTINGS"):  # key, display name, type, default: for writing the docs
    for s in engine.GetRendererSettingsList():
        print("\t".join([str(s.key), str(s.name), str(s.type).split(".")[-1], str(s.defValue)]))

docs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "settings.md")
with open(docs, encoding="utf-8") as f:
    documented = set(re.findall(r"`(eldr:\w+)`", f.read()))
missing = [k for k in keys if k not in documented]
unknown = sorted(documented - set(keys))  # a renamed or removed setting left behind
for k in missing:
    print("FAIL: %s is not in docs/settings.md" % k, file=sys.stderr)
for k in unknown:
    print("FAIL: docs/settings.md names %s, which the renderer doesn't have" % k, file=sys.stderr)
del engine
print("FAILED" if missing or unknown else "OK (%d settings)" % len(keys))
sys.exit(1 if missing or unknown else 0)
