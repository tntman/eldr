"""Every eldr: setting the renderer exposes is documented in docs/settings.md,
in GROUPS' order, with a "What it does" text short enough for a tooltip.
Run: usdenv.cmd python tests\test_docs.py"""
import os
import re
import sys

from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from pxr import UsdImagingGL
from pxr.Usdviewq.qt import QtWidgets

from hdvkUsdview.settings import GROUPS
from hdvkUsdview.tooltips import MAX, Tooltips

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
    text = f.read()
documented = set(re.findall(r"`(eldr:\w+)`", text))
missing = [k for k in keys if k not in documented]
unknown = sorted(documented - set(keys))  # a renamed or removed setting left behind
for k in missing:
    print("FAIL: %s is not in docs/settings.md" % k, file=sys.stderr)
for k in unknown:
    print("FAIL: docs/settings.md names %s, which the renderer doesn't have" % k, file=sys.stderr)
bad = []
rows = re.findall(r"^\| [^|]*<br>`(eldr:\w+)` \|", text, re.M)  # table rows, in document order
want = ["eldr:" + k for _, _, ks in GROUPS for k in ks.split()]
if rows != want:
    bad.append("docs/settings.md rows are not in GROUPS' order (first difference: %s)" %
               next((a for a, b in zip(rows, want) if a != b), "length"))
for k, tip in Tooltips(docs).items():
    if not tip or len(tip) > MAX:
        bad.append("%s: tooltip is %d characters (1-%d): shorten its row" % (k, len(tip), MAX))
for b in bad:
    print("FAIL:", b, file=sys.stderr)
del engine
failed = missing or unknown or bad
print("FAILED" if failed else "OK (%d settings)" % len(keys))
sys.exit(1 if failed else 0)
