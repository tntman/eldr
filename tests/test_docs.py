"""Every eldr: setting the renderer exposes is documented in docs/settings.md,
in hydra/renderSettings.def's order under its group's heading, with the
renderer's default and a "What it does" text short enough for a tooltip.
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
settings = [s for s in engine.GetRendererSettingsList() if str(s.key).startswith("eldr:")]
keys = [str(s.key) for s in settings]
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
# Table rows by heading ("## Advanced" + "### Shadows" is "Advanced: Shadows"), in document order.
docGroups, h2, defaults = [], "", {}
for line in text.splitlines():
    heading = re.match(r"(##+) (.*)", line)
    row = re.match(r"\| [^|]*<br>`(eldr:\w+)` \| ([^|]*)\|", line)
    if heading:
        h2 = heading.group(2) if heading.group(1) == "##" else h2
        title = heading.group(2) if heading.group(1) == "##" else h2 + ": " + heading.group(2)
    elif row:
        if not docGroups or docGroups[-1][0] != title:
            docGroups.append((title, []))
        docGroups[-1][1].append(row.group(1)[5:])
        defaults[row.group(1)] = row.group(2).strip()
want = [(title, keys.split()) for title, _, keys in GROUPS]
if docGroups != want:
    diff = next((a, b) for a, b in zip(docGroups + [None], want + [None]) if a != b)
    bad.append("docs/settings.md's tables are not renderSettings.def's groups (first difference: docs %s, def %s)" % diff)


def Shown(v):
    """How the Default column starts for the renderer's default v."""
    if isinstance(v, bool):
        return ["on" if v else "off"]
    if isinstance(v, int):
        return ["int %d" % v]
    if isinstance(v, float):
        return ["float %g" % v]
    return ["string empty"] if v == "" else ["string " + v, 'string "%s"' % v]


for s in settings:
    cell = defaults.get(str(s.key), "")
    if not any(cell == w or cell.startswith(w + " ") for w in Shown(s.defValue)):
        bad.append("%s: docs default '%s', renderer's %s" % (s.key, cell, Shown(s.defValue)[0]))
for k, tip in Tooltips(docs).items():
    if not tip or len(tip) > MAX:
        bad.append("%s: tooltip is %d characters (1-%d): shorten its row" % (k, len(tip), MAX))
for b in bad:
    print("FAIL:", b, file=sys.stderr)
del engine
failed = missing or unknown or bad
print("FAILED" if failed else "OK (%d settings)" % len(keys))
sys.exit(1 if failed else 0)
