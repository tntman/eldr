"""usdview plugin: Eldr > Attribute Editor, a window of the selected prim's
attributes whose values can be edited. Edits go to the session layer (File >
Save Overrides As keeps them); right-click > Revert removes one."""

import json

from pxr import Sdf, Usd
from pxr.Usdviewq.common import GetPropertyColor
from pxr.Usdviewq.qt import QtCore, QtWidgets

# Longer arrays (points, normals) are shown as their length, read-only.
MAX_ARRAY = 16

_BARE = (Sdf.ValueTypeNames.String, Sdf.ValueTypeNames.Token)


def Text(value, typeName):
    """A value as the editor shows it: usda syntax, but strings, tokens and
    asset paths bare."""
    if value is None:
        return ""
    if isinstance(value, Sdf.AssetPath):
        return value.path
    if isinstance(value, str):
        return value
    if typeName.isArray:
        if len(value) > MAX_ARRAY:
            return "[%d values]" % len(value)
        if typeName.scalarType in _BARE:
            return json.dumps(list(value))
    if isinstance(value, float) and typeName in (Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Half):
        return "%.7g" % value  # a float32, not the double Python holds
    return str(value)


def Value(text, typeName):
    """Text from the editor as a value of typeName; ValueError if it isn't one."""
    if typeName in _BARE:
        return text
    if typeName == Sdf.ValueTypeNames.Asset:
        return Sdf.AssetPath(text)
    # The rest is read as usda reads it: (1, 0.5, 0) for a color3f etc.
    layer = Sdf.Layer.CreateAnonymous(".usda")
    try:
        if layer.ImportFromString('#usda 1.0\nover "x" {\n%s a = %s\n}\n' % (typeName, text)):
            return layer.GetAttributeAtPath("/x.a").default
    except Exception:
        pass
    raise ValueError("Not a %s: %s" % (typeName, text))


def Edited(attr):
    """Whether the session layer says something about attr."""
    return bool(attr.GetStage().GetSessionLayer().GetAttributeAtPath(attr.GetPath()))


def Set(attr, value):
    """Authors value on attr in the session layer; None removes what the
    session layer says about attr (back to the file's value).
    ponytail: a default there, which an animated attribute holds at every
    frame; author time samples if editing animation is wanted."""
    stage = attr.GetStage()
    if value is None:
        spec = stage.GetSessionLayer().GetAttributeAtPath(attr.GetPath())
        if spec:
            spec.owner.RemoveProperty(spec)
        return
    with Usd.EditContext(stage, stage.GetSessionLayer()):
        if not attr.Set(value):
            raise ValueError("%s can't be set" % attr.GetName())


class _AttributeWindow(QtWidgets.QWidget):
    def __init__(self, usdviewApi):
        super().__init__(usdviewApi.qMainWindow, QtCore.Qt.Tool)
        self.setWindowTitle("Eldr Attribute Editor")
        self.resize(520, 640)
        self._model = usdviewApi.dataModel
        self._attrs = []
        self._path = QtWidgets.QLabel()
        self._filter = QtWidgets.QLineEdit()
        self._filter.setPlaceholderText("Filter")
        self._table = QtWidgets.QTableWidget(0, 3)
        self._table.setHorizontalHeaderLabels(["Name", "Type", "Value"])
        self._table.verticalHeader().hide()
        header = self._table.horizontalHeader()
        header.setSectionResizeMode(0, QtWidgets.QHeaderView.ResizeToContents)
        header.setSectionResizeMode(1, QtWidgets.QHeaderView.ResizeToContents)
        header.setStretchLastSection(True)
        self._table.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self._status = QtWidgets.QLabel()
        self._status.setWordWrap(True)
        layout = QtWidgets.QVBoxLayout(self)
        for w in (self._path, self._filter, self._table, self._status):
            layout.addWidget(w)
        # Changes arrive in bursts (playback, an edit's own notices): one
        # rebuild once they settle.
        self._refresh = QtCore.QTimer(self)
        self._refresh.setSingleShot(True)
        self._refresh.timeout.connect(self.Rebuild)
        for signal in (self._model.selection.signalPrimSelectionChanged, self._model.currentFrameChanged,
                       self._model.signalStageReplaced, self._model.signalPrimsChanged):
            signal.connect(lambda *_: self._refresh.start(0))
        self._table.itemChanged.connect(self._Edited)
        self._table.customContextMenuRequested.connect(self._Menu)
        self._filter.textChanged.connect(self._Filter)

    def Rebuild(self):
        if not self.isVisible() or self._table.state() == QtWidgets.QAbstractItemView.EditingState:
            return  # showing it, or the edit's commit, rebuilds
        prim = self._model.selection.getFocusPrim()
        prim = prim if prim and prim.IsValid() else None
        self._path.setText(str(prim.GetPath()) if prim else "No prim selected")
        self._attrs = prim.GetAttributes() if prim else []
        time = self._model.currentFrame
        self._table.blockSignals(True)
        self._table.setRowCount(0)
        self._table.setRowCount(len(self._attrs))
        for row, attr in enumerate(self._attrs):
            typeName = attr.GetTypeName()
            value = attr.Get(time)
            name = QtWidgets.QTableWidgetItem(attr.GetName())
            name.setForeground(GetPropertyColor(attr, time))  # usdview's: fallback, default, animated
            if Edited(attr):
                font = name.font()
                font.setBold(True)  # edited here
                name.setFont(font)
            cells = [name, QtWidgets.QTableWidgetItem(str(typeName)), QtWidgets.QTableWidgetItem()]
            for cell in cells[:2]:
                cell.setFlags(cell.flags() & ~QtCore.Qt.ItemIsEditable)
            allowed = attr.GetMetadata("allowedTokens")
            if typeName == Sdf.ValueTypeNames.Bool:
                cells[2].setFlags(cells[2].flags() & ~QtCore.Qt.ItemIsEditable | QtCore.Qt.ItemIsUserCheckable)
                cells[2].setCheckState(QtCore.Qt.Checked if value else QtCore.Qt.Unchecked)
            elif allowed and not typeName.isArray:
                combo = QtWidgets.QComboBox()
                combo.addItems(list(allowed) if value in allowed else [value or ""] + list(allowed))
                combo.setCurrentText(value or "")
                combo.currentTextChanged.connect(lambda text, a=attr: self._Set(a, text))
                self._table.setCellWidget(row, 2, combo)
            else:
                cells[2].setText(Text(value, typeName))
                if typeName.isArray and value is not None and len(value) > MAX_ARRAY:
                    cells[2].setFlags(cells[2].flags() & ~QtCore.Qt.ItemIsEditable)
            for column, cell in enumerate(cells):
                self._table.setItem(row, column, cell)
        self._table.blockSignals(False)
        self._Filter()

    def _Filter(self):
        text = self._filter.text().lower()
        for row, attr in enumerate(self._attrs):
            self._table.setRowHidden(row, text not in attr.GetName().lower())

    def _Edited(self, item):
        if item.column() != 2:
            return
        attr = self._attrs[item.row()]
        if attr.GetTypeName() == Sdf.ValueTypeNames.Bool:
            self._Set(attr, item.checkState() == QtCore.Qt.Checked)
            return
        try:
            value = Value(item.text(), attr.GetTypeName())
        except ValueError as e:
            self._status.setText(str(e))
            self._refresh.start(0)  # back to the attribute's value
            return
        self._Set(attr, value)

    def _Set(self, attr, value):
        try:
            Set(attr, value)
            self._status.setText("")
        except Exception as e:
            # Tf errors carry the C++ location first; the reason is last.
            self._status.setText(str(e).strip().splitlines()[-1].strip(" '"))
        self._refresh.start(0)

    def _Menu(self, pos):
        row = self._table.rowAt(pos.y())
        if row < 0:
            return
        attr = self._attrs[row]
        menu = QtWidgets.QMenu(self)
        revert = menu.addAction("Revert")
        revert.setEnabled(Edited(attr))
        if menu.exec_(self._table.viewport().mapToGlobal(pos)) == revert:
            self._Set(attr, None)
