"""usdview plugin: Eldr > Render Statistics opens a window of what Eldr's
GetRenderStats() reports about the last frame, refreshed twice a second;
Eldr > Attribute Editor edits the selected prim's attributes (attributes.py);
Eldr > Render Settings shows every setting in groups (settings.py).
usdenv.cmd puts it on the plugin and Python paths (plugInfo.json beside it)."""

from pxr import Tf
from pxr.Usdviewq.common import ReportMetricSize
from pxr.Usdviewq.plugin import PluginContainer
from pxr.Usdviewq.qt import QtCore, QtWidgets

# GPU passes in the order HdVk runs them (renderer.cpp's pass names).
PASSES = [
    ("probeCapture", "Reflection probe capture"),
    ("planarReflection", "Planar reflection"),
    ("shutter", "Shutter (motion blur samples)"),
    ("culling", "Culling, skinning"),
    ("lightClusters", "Light clusters"),
    ("sunShadows", "Sun shadows"),
    ("sunShadowMin", "Sun shadow minimum"),
    ("depthPrepass", "Depth prepass"),
    ("ambientOcclusion", "Ambient occlusion"),
    ("ssgi", "Screen-space GI"),
    ("localShadows", "Local light shadows"),
    ("fog", "Volumetric fog"),
    ("accelerationStructures", "Acceleration structures (TLAS)"),
    ("giProbes", "Global illumination probes"),
    ("rayTracing", "Ray-traced shadows, reflections"),
    ("shading", "Shading"),
    ("ssrDenoise", "Reflection denoise"),
    ("subsurface", "Subsurface scattering"),
    ("volumes", "Volumes"),
    ("translucency", "Translucency (refraction, OIT)"),
    ("volumesInFront", "Volumes in front of translucency"),
    ("lightShapes", "Light shapes"),
    ("depthOfField", "Depth of field"),
    ("film", "Film accumulation"),
    ("taa", "TAA"),
    ("motionBlur", "Motion blur"),
    ("post", "Bloom, exposure, tonemap"),
    ("aovs", "Extra AOVs"),
    ("lensDistortion", "Lens distortion"),
    ("readback", "Readback to Hydra"),
]


# What the window shows, in order: (heading, [(label, GetRenderStats() key,
# unit)]). Units: ms, n (count), B (bytes), EV, % and str. A dotted key reaches
# into a dictionary (gpuPassMs.shading). houdini/gen_ui.py reads this table
# (and PASSES) to write the stats Houdini's Render Statistics pane lists
# (houdini/UsdRenderers.json), so the two cannot drift.
STATS = [
    ("Frame", [("GPU time", "gpuTimeMs", "ms"),
               ("Render call (CPU, waits for GPU)", "renderTimeMs", "ms"),
               ("Scene upload (CPU)", "commitTimeMs", "ms"),
               ("Frame to the viewport", "frameSharing", "str")]),
    ("GPU passes", "PASSES"),
    ("Drawn (after culling)", [("Triangles drawn", "trianglesDrawn", "n"),
                               ("Triangles in scene", "trianglesTotal", "n"),
                               ("Instances drawn", "instancesDrawn", "n"),
                               ("Instances in scene", "instancesTotal", "n"),
                               ("Meshes", "meshes", "n"),
                               ("Volumes", "volumes", "n"),
                               ("Draw calls", "drawCalls", "n")]),
    ("Image", [("Stage", "rendererStage", "str"),
               ("Samples done", "numCompletedSamples", "n"),
               ("Samples target", "taaTarget", "n"),
               ("Pixels converged", "pixelsConvergedPercent", "%"),
               ("Time to converge", "totalClockTime", "s"),
               ("Exposure", "exposureEV", "EV")]),
    # Graph nodes Eldr couldn't compile (bypassed or dropped) and unknown surfaces.
    ("Materials", [("Inputs fallen back", "materialFallbackInputs", "n"),
                   ("Nodes not compiled", "materialFallbackNodes", "str")]),
    ("Memory", [("Graphics memory (HdVk)", "gpuMemoryUsed", "B"),
                ("  textures", "textureMemory", "B"),
                ("  vertices, instances", "primvar", "B"),
                ("  indices", "topology", "B"),
                ("  acceleration structures", "accelerationStructureMemory", "B"),
                ("  volumes", "volumeMemory", "B"),
                ("  peak", "peakMemory", "B"),
                ("System memory the GPU reads", "hostMemoryUsed", "B"),
                ("Card: this process", "vramUsage", "B"),
                ("Card: budget", "vramBudget", "B"),
                ("Card: size", "vramSize", "B")]),
]


def Lookup(stats, key):
    for part in key.split("."):
        stats = stats.get(part) if isinstance(stats, dict) else None
    return stats


def Format(value, unit):
    if unit == "ms":
        return "%.2f ms" % value
    if unit == "n":
        return "{:,}".format(int(value)).replace(",", " ")
    if unit == "B":
        return ReportMetricSize(value)
    if unit == "EV":
        return "%+.2f EV" % value
    if unit == "%":
        return "%.0f %%" % value
    if unit == "s":
        return "%.2f s" % value
    return str(value)


def Rows(stats):
    """The window's (label, value) rows; a row without a value is a heading."""
    if stats.get("rendererName") != "Eldr":
        return [("Eldr is not the viewport's renderer.", "")]
    rows = []
    for heading, entries in STATS:
        rows.append((heading, None))
        if entries == "PASSES":
            entries = [(label, "gpuPassMs." + key, "ms") for key, label in PASSES]
        for label, key, unit in entries:
            value = Lookup(stats, key)
            if value is not None and not (key.startswith("vram") and not stats.get("vramSize")):
                rows.append((label, Format(value, unit)))
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
_settingsWindow = None


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


def _ShowSettings(usdviewApi):
    global _settingsWindow
    if _settingsWindow is None:
        from .settings import _SettingsWindow
        _settingsWindow = _SettingsWindow(usdviewApi)
    _settingsWindow.Rebuild()
    _settingsWindow.show()
    _settingsWindow.raise_()


class HdVkContainer(PluginContainer):
    def registerPlugins(self, plugRegistry, usdviewApi):
        self._stats = plugRegistry.registerCommandPlugin(
            "HdVkContainer.renderStatistics", "Render Statistics", _ShowStats,
            "HdVk's GPU time per pass, what it drew, memory and convergence")
        self._attributes = plugRegistry.registerCommandPlugin(
            "HdVkContainer.attributeEditor", "Attribute Editor", _ShowAttributes,
            "Edit the selected prim's attribute values (in the session layer)")
        self._settings = plugRegistry.registerCommandPlugin(
            "HdVkContainer.renderSettings", "Render Settings", _ShowSettings,
            "Every Eldr render setting, grouped, with a filter")

    def configureView(self, plugRegistry, plugUIBuilder):
        menu = plugUIBuilder.findOrCreateMenu("Eldr")
        menu.addItem(self._settings)
        menu.addItem(self._stats)
        menu.addItem(self._attributes)


Tf.Type.Define(HdVkContainer)
