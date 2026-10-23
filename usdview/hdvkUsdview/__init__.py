"""usdview plugin: HdVk > Render Statistics opens a window of what HdVk's
GetRenderStats() reports about the last frame, refreshed twice a second.
usdenv.cmd puts it on the plugin and Python paths (plugInfo.json beside it)."""

from pxr import Tf
from pxr.Usdviewq.common import ReportMetricSize
from pxr.Usdviewq.plugin import PluginContainer
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

# GPU passes in the order HdVk runs them (renderer.cpp's pass names).
PASSES = [
    ("culling", "Culling, skinning"),
    ("lightClusters", "Light clusters"),
    ("sunShadows", "Sun shadows"),
    ("depthPrepass", "Depth prepass"),
    ("ambientOcclusion", "Ambient occlusion"),
    ("localShadows", "Local light shadows"),
    ("shading", "Shading"),
    ("depthOfField", "Depth of field"),
    ("taa", "TAA"),
    ("motionBlur", "Motion blur"),
    ("post", "Bloom, exposure, tonemap"),
    ("readback", "Readback to Hydra"),
]


def Rows(stats):
    """The window's (label, value) rows; a row without a value is a heading."""
    if stats.get("rendererName") != "HdVk":
        return [("HdVk is not the viewport's renderer.", "")]
    get = lambda key: stats.get(key, 0)
    ms = lambda key: "%.2f ms" % get(key)
    count = lambda key: "{:,}".format(get(key)).replace(",", " ")
    rows = [("Frame", None),
            ("GPU", ms("gpuTimeMs")),
            ("Render call (CPU, waits for GPU)", ms("renderTimeMs")),
            ("Scene upload (CPU)", ms("commitTimeMs")),
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
             ("  peak", ReportMetricSize(get("peakMemory"))),
             ("System memory the GPU reads", ReportMetricSize(get("hostMemoryUsed")))]
    if get("vramSize"):
        rows.append(("Card: this process / budget / size",
                     " / ".join(ReportMetricSize(get(k)) for k in ("vramUsage", "vramBudget", "vramSize"))))
    return rows


class _StatsWindow(QtWidgets.QWidget):
    def __init__(self, usdviewApi):
        super().__init__(usdviewApi.qMainWindow, QtCore.Qt.Tool)
        self.setWindowTitle("HdVk Render Statistics")
        self._api = usdviewApi
        self._label = QtWidgets.QLabel()
        self._label.setFont(QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont))
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
        rows = Rows(engine.GetRenderStats() if engine else {})
        width = max(len(label) for label, _ in rows)
        self._label.setText("\n".join(label if value is None else "  %-*s  %s" % (width, label, value)
                                      for label, value in rows))


_window = None


def _ShowStats(usdviewApi):
    global _window
    if _window is None:
        _window = _StatsWindow(usdviewApi)
    _window.show()
    _window.raise_()


class HdVkContainer(PluginContainer):
    def registerPlugins(self, plugRegistry, usdviewApi):
        self._stats = plugRegistry.registerCommandPlugin(
            "HdVkContainer.renderStatistics", "Render Statistics", _ShowStats,
            "HdVk's GPU time per pass, what it drew, memory and convergence")

    def configureView(self, plugRegistry, plugUIBuilder):
        plugUIBuilder.findOrCreateMenu("HdVk").addItem(self._stats)


Tf.Type.Define(HdVkContainer)
