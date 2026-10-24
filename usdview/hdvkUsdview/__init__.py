"""usdview plugin: Eldr > Render Statistics opens a window of what Eldr's
GetRenderStats() reports about the last frame, refreshed twice a second;
Eldr > Attribute Editor edits the selected prim's attributes (attributes.py).
usdenv.cmd puts it on the plugin and Python paths (plugInfo.json beside it)."""

from pxr import Tf
from pxr.Usdviewq.common import ReportMetricSize
from pxr.Usdviewq.plugin import PluginContainer
from pxr.Usdviewq.qt import QtCore, QtWidgets

# GPU passes in the order HdVk runs them (renderer.cpp's pass names).
PASSES = [
    ("probeCapture", "Reflection probe capture"),
    ("planarReflection", "Planar reflection"),
    ("culling", "Culling, skinning"),
    ("lightClusters", "Light clusters"),
    ("sunShadows", "Sun shadows"),
    ("depthPrepass", "Depth prepass"),
    ("ambientOcclusion", "Ambient occlusion"),
    ("ssgi", "Screen-space GI"),
    ("localShadows", "Local light shadows"),
    ("fog", "Volumetric fog"),
    ("accelerationStructures", "Acceleration structures (TLAS)"),
    ("giProbes", "Global illumination probes"),
    ("rayTracing", "Ray-traced shadows, reflections"),
    ("shading", "Shading"),
    ("subsurface", "Subsurface scattering"),
    ("translucency", "Translucency (refraction, OIT)"),
    ("volumes", "Volumes"),
    ("depthOfField", "Depth of field"),
    ("taa", "TAA"),
    ("motionBlur", "Motion blur"),
    ("post", "Bloom, exposure, tonemap"),
    ("aovs", "Extra AOVs"),
    ("lensDistortion", "Lens distortion"),
    ("readback", "Readback to Hydra"),
]


def Rows(stats):
    """The window's (label, value) rows; a row without a value is a heading."""
    if stats.get("rendererName") != "Eldr":
        return [("Eldr is not the viewport's renderer.", "")]
    get = lambda key: stats.get(key, 0)
    ms = lambda key: "%.2f ms" % get(key)
    count = lambda key: "{:,}".format(get(key)).replace(",", " ")
    rows = [("Frame", None),
            ("GPU", ms("gpuTimeMs")),
            ("Render call (CPU, waits for GPU)", ms("renderTimeMs")),
            ("Scene upload (CPU)", ms("commitTimeMs")),
            ("Frame to the viewport", "GPU (OpenGL)" if stats.get("frameSharing") == "OpenGL" else "CPU readback"),
            ("GPU passes", None)]
    passes = stats.get("gpuPassMs", {})
    rows += [(label, "%.2f ms" % passes[key]) for key, label in PASSES if key in passes]
    rows += [("Drawn (after culling)", None),
             ("Triangles", count("trianglesDrawn") + " of " + count("trianglesTotal")),
             ("Instances", count("instancesDrawn") + " of " + count("instancesTotal")),
             ("Meshes", count("meshes")),
             ("Draw calls", count("drawCalls")),
             ("Image", None),
             ("TAA samples", "%d of %d (%s)" % (get("numCompletedSamples"), get("taaTarget"),
                                                stats.get("rendererStage", ""))),
             ("Exposure", "%+.2f EV" % get("exposureEV")),
             ("Memory", None),
             ("Graphics memory (HdVk)", ReportMetricSize(get("gpuMemoryUsed"))),
             ("  textures", ReportMetricSize(get("textureMemory"))),
             ("  vertices, instances", ReportMetricSize(get("primvar"))),
             ("  indices", ReportMetricSize(get("topology"))),
             ("  acceleration structures", ReportMetricSize(get("accelerationStructureMemory"))),
             ("  volumes", ReportMetricSize(get("volumeMemory"))),
             ("  peak", ReportMetricSize(get("peakMemory"))),
             ("System memory the GPU reads", ReportMetricSize(get("hostMemoryUsed")))]
    if get("vramSize"):
        rows.append(("Card: this process / budget / size",
                     " / ".join(ReportMetricSize(get(k)) for k in ("vramUsage", "vramBudget", "vramSize"))))
    return rows


def Html(rows):
    """Rows as a two-column table, headings bold, values right-aligned.
    usdview's style sheet replaces the font, so spaces can't line them up."""
    from html import escape
    out = ['<table cellspacing="0" cellpadding="1">']
    for i, (label, value) in enumerate(rows):
        if value is None:
            gap = "" if i == 0 else '<tr><td colspan="2" style="font-size:4px">&nbsp;</td></tr>'
            out.append('%s<tr><td colspan="2"><b>%s</b></td></tr>' % (gap, escape(label)))
        else:
            indent = "&nbsp;" * (8 if label.startswith("  ") else 4)
            out.append('<tr><td>%s%s&nbsp;&nbsp;&nbsp;&nbsp;</td><td align="right">%s</td></tr>'
                       % (indent, escape(label.strip()), escape(value)))
    out.append("</table>")
    return "".join(out)


class _StatsWindow(QtWidgets.QWidget):
    def __init__(self, usdviewApi):
        super().__init__(usdviewApi.qMainWindow, QtCore.Qt.Tool)
        self.setWindowTitle("Eldr Render Statistics")
        self._api = usdviewApi
        self._label = QtWidgets.QLabel()
        self._label.setTextFormat(QtCore.Qt.RichText)
        self._label.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        layout = QtWidgets.QVBoxLayout(self)
        layout.addWidget(self._label)
        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self._Refresh)
        self._timer.start(500)
        self._Refresh()

    def _Refresh(self):
        # usdview has no public way to the engine: its StageView's.
        from pxr.Usdviewq.stageView import StageView
        view = self._api.qMainWindow.findChild(StageView)
        engine = view._getRenderer() if view else None
        self._label.setText(Html(Rows(engine.GetRenderStats() if engine else {})))


_window = None
_attributeWindow = None


def _ShowStats(usdviewApi):
    global _window
    if _window is None:
        _window = _StatsWindow(usdviewApi)
    _window.show()
    _window.raise_()


def _ShowAttributes(usdviewApi):
    global _attributeWindow
    if _attributeWindow is None:
        from .attributes import _AttributeWindow
        _attributeWindow = _AttributeWindow(usdviewApi)
    _attributeWindow.show()
    _attributeWindow.raise_()
    _attributeWindow.Rebuild()


class HdVkContainer(PluginContainer):
    def registerPlugins(self, plugRegistry, usdviewApi):
        self._stats = plugRegistry.registerCommandPlugin(
            "HdVkContainer.renderStatistics", "Render Statistics", _ShowStats,
            "HdVk's GPU time per pass, what it drew, memory and convergence")
        self._attributes = plugRegistry.registerCommandPlugin(
            "HdVkContainer.attributeEditor", "Attribute Editor", _ShowAttributes,
            "Edit the selected prim's attribute values (in the session layer)")

    def configureView(self, plugRegistry, plugUIBuilder):
        menu = plugUIBuilder.findOrCreateMenu("Eldr")
        menu.addItem(self._stats)
        menu.addItem(self._attributes)


Tf.Type.Define(HdVkContainer)
