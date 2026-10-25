"""usdview plugin: Eldr > Render Settings, every Eldr render setting in
groups that scroll, with a filter. usdview's own Hydra Settings > More...
dialog lists them in one column that runs off the screen."""

from pxr.Usdviewq.qt import QtCore, QtWidgets

# Groups in the order the window shows them; settings not listed (new ones)
# land in "Other".
GROUPS = [
    ("Performance and geometry",
     "depthPrepass frustumCulling cullPixels lod lodBias lodFade subdivision maxRefineLevel displacement curveRibbons "
     "pointSprites displacementScale displacementGraphs anisotropy shareFrame extraAovs splitDomeDirect renderProducts productLinear pipelineCache "
     "pipelinePrewarm"),
    ("Lighting",
     "imageBasedLighting localLights clusteredLights lightTextures emissiveLights emissiveLightThreshold domeSun domeSunThreshold ambientOcclusion "
     "aoRadius aoSlices gi giProbes giRaysPerProbe giHysteresis giPreconverge ssgi ssgiSteps ssgiRadius ssgiResolution "
     "ssgiThickness ssgiBitmask ssgiBitmaskThickness directClamp indirectClamp"),
    ("Shadows",
     "shadows shadowResolution shadowCascades localShadows localShadowResolution softShadows softSunShadows "
     "softShadowSamples translucentShadows translucentShadowDivisor rtShadows rtShadowSamples rtSkinnedInstances "
     "rtDisplacement shadowJitter"),
    ("Reflections",
     "ssr ssrSteps ssrDenoise rtReflections rtReflectionSamples reflectionProbe probeResolution probePosition authoredProbes "
     "probeNormalize planarReflections planarScale planarCount"),
    ("Materials",
     "normalMaps specularWorkflow clearcoat sheen thinFilm anisotropicSpecular hairShading toon toonBands texturedLobes "
     "materialGraphs proceduralTextures subsurface subsurfaceSamples"),
    ("Transparency", "texturedTransmission translucency oit oitDepthWeight refraction refractionSamples holdouts refractionBackFaces multiRefraction refractionLayers holdoutAlpha"),
    ("Fog and volumes",
     "fog fogDensity fogHeight fogHeightFalloff fogAnisotropy fogGiAmbient fogColor fogMaxDistance fogGridSize fogZUp fogFilmJitter "
     "volumes volumeStepSize volumeMaxSteps volumeShadowSteps volumeDensity volumeEmission volumeAnisotropy "
     "volumeResolution volumeUnderTransparency volumeShadows"),
    ("Camera and image",
     "taa taaSamples renderScale filmAccumulation filmSamples filmFilterRadius finalQuality sampleDimensions interactiveSamples adaptiveThreshold adaptiveFinalThreshold adaptivePixelFraction adaptiveMinSamples objectMotion blendShapeMotion depthOfField dofJitter dofResidualBlur motionBlur motionBlurTimeSamples sampledMotion deformationBlur lensDistortion postProcess "
     "autoExposure exposureCompensation bloomIntensity tonemapper"),
]


def Groups(settings):
    """[(group, [setting...])] of the renderer's setting descriptors (key,
    name, type, defValue), in GROUPS' order, then "Other"."""
    where = {}
    for i, (_, keys) in enumerate(GROUPS):
        for k in keys.split():
            where["hdvk:" + k] = i
    out = [(title, []) for title, _ in GROUPS] + [("Other", [])]
    for s in settings:
        out[where.get(str(s.key), len(GROUPS))][1].append(s)
    return [g for g in out if g[1]]


def Kind(setting):
    """FLAG, INT, FLOAT or STRING."""
    return str(setting.type).split(".")[-1]


class _SettingsWindow(QtWidgets.QWidget):
    def __init__(self, usdviewApi, engine=None):
        super().__init__(usdviewApi.qMainWindow, QtCore.Qt.Tool)
        self.setWindowTitle("Eldr Render Settings")
        self.resize(680, 760)
        self._api = usdviewApi
        self._engine = engine  # tests pass one; usdview's comes from its view
        self._rows = []        # (setting, label, editor, row widget)
        self._filter = QtWidgets.QLineEdit()
        self._filter.setPlaceholderText("Filter")
        self._filter.textChanged.connect(self._Filter)
        self._area = QtWidgets.QScrollArea()
        self._area.setWidgetResizable(True)
        reset = QtWidgets.QPushButton("Reset All to Defaults")
        reset.clicked.connect(self._ResetAll)
        layout = QtWidgets.QVBoxLayout(self)
        layout.addWidget(self._filter)
        layout.addWidget(self._area)
        layout.addWidget(reset)

    def _Engine(self):
        if self._engine:
            return self._engine
        from pxr.Usdviewq.stageView import StageView
        view = self._api.qMainWindow.findChild(StageView)
        return view._getRenderer() if view else None

    def _Redraw(self):
        from pxr.Usdviewq.stageView import StageView
        view = self._api.qMainWindow.findChild(StageView) if self._api.qMainWindow else None
        if view:
            view.update()

    def Rebuild(self):
        engine = self._Engine()
        settings = [s for s in engine.GetRendererSettingsList() if str(s.key).startswith("hdvk:")] if engine else []
        page = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(page)
        self._rows = []
        if not settings:
            column.addWidget(QtWidgets.QLabel("Eldr is not the viewport's renderer."))
        for title, group in Groups(settings):
            box = QtWidgets.QGroupBox(title)
            form = QtWidgets.QFormLayout(box)
            for s in group:
                label = QtWidgets.QLabel(s.name)
                label.setWordWrap(True)
                label.setMinimumWidth(300)  # names stay on one or two lines
                editor = self._Editor(s, engine)
                form.addRow(label, editor)
                self._rows.append((s, label, editor, box))
            column.addWidget(box)
        column.addStretch()
        self._area.setWidget(page)
        self._Mark()
        self._Filter()

    def _Value(self, s, engine):
        v = engine.GetRendererSetting(s.key)
        return s.defValue if v is None else v

    def _Editor(self, s, engine):
        kind, value = Kind(s), self._Value(s, engine)
        if kind == "FLAG":
            w = QtWidgets.QCheckBox()
            w.setChecked(bool(value))
            w.toggled.connect(lambda on, s=s: self._Set(s, bool(on)))
        elif kind == "INT":
            w = QtWidgets.QSpinBox()
            w.setRange(-1000000, 1000000)
            w.setValue(int(value))
            w.valueChanged.connect(lambda v, s=s: self._Set(s, int(v)))
        elif kind == "FLOAT":
            w = QtWidgets.QDoubleSpinBox()
            w.setRange(-1e6, 1e6)
            w.setDecimals(3)
            w.setSingleStep(0.1)
            w.setValue(float(value))
            w.valueChanged.connect(lambda v, s=s: self._Set(s, float(v)))
        else:
            w = QtWidgets.QLineEdit(str(value))
            w.editingFinished.connect(lambda s=s, w=w: self._Set(s, w.text()))
        return w

    def _Set(self, s, value):
        engine = self._Engine()
        if engine:
            engine.SetRendererSetting(s.key, value)
            self._Mark()
            self._Redraw()

    def _Mark(self):
        # Changed settings in bold, so what differs from the defaults shows.
        engine = self._Engine()
        for s, label, _, _ in self._rows:
            font = label.font()
            font.setBold(bool(engine) and self._Value(s, engine) != s.defValue)
            label.setFont(font)

    def _Filter(self):
        text = self._filter.text().lower()
        shown = set()
        for s, label, editor, box in self._rows:
            visible = text in s.name.lower() or text in str(s.key).lower() or text in box.title().lower()
            label.setVisible(visible)
            editor.setVisible(visible)
            if visible:
                shown.add(box)
        for _, _, _, box in self._rows:
            box.setVisible(box in shown)

    def _ResetAll(self):
        engine = self._Engine()
        if engine:
            for s, _, _, _ in self._rows:
                engine.SetRendererSetting(s.key, s.defValue)
        self.Rebuild()
        self._Redraw()
