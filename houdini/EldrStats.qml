// Eldr's render statistics panel (Render Gallery / viewport stats). Reads the
// `stats` context property: the delegate's GetRenderStats() live, or the
// husk:render_stats of an EXR. Every key is optional (live and EXR data
// differ); a missing one hides its tile. Units: bytes and milliseconds as in
// hydra/renderDelegate.cpp. Houdini's own components (houdini.ui, houdini.info,
// houdini.renderstats) do the drawing.
import QtQuick
import houdini.pluto
import houdini.ui
import houdini.info
import houdini.renderstats

Item {
    id: root

    readonly property var sd: stats ? stats : ({})
    readonly property real hSpacing: 8
    readonly property real availableWidth: root.width - (col.leftPadding + col.rightPadding)
    readonly property real halfWidth: (root.availableWidth - root.hSpacing) / 2
    readonly property real fractionDone: sd.fractionDone !== undefined ? sd.fractionDone
        : (sd.percentDone !== undefined ? sd.percentDone / 100.0 : 0.0)
    readonly property real elapsed: sd.totalClockTime !== undefined ? sd.totalClockTime : 0.0
    readonly property bool running: fractionDone > 0.0 && fractionDone < 1.0
    readonly property int fallbacks: sd.materialFallbackInputs !== undefined ? sd.materialFallbackInputs : 0

    // (houdini.ui has a Number component: JavaScript's Number() is shadowed, use unary plus.)
    function num(key) { return sd[key] !== undefined && sd[key] !== null ? +sd[key] : 0 }
    function has(key) { return sd[key] !== undefined && sd[key] !== null }
    function ms(v) { return v >= 100 ? v.toFixed(0) + " ms" : v.toFixed(v >= 10 ? 1 : 2) + " ms" }
    function pct(a, b) { return b > 0 ? Math.round(100 * a / b) + "%" : "-" }

    // The GPU passes, biggest first: the top 3 and the rest as "other".
    readonly property var passModel: {
        const p = sd.gpuPassMs
        if (!p) return null
        const names = Object.keys(p).filter(k => p[k] > 0).sort((a, b) => p[b] - p[a])
        const out = {}
        let other = 0
        names.forEach((k, i) => { if (i < 3) out[k] = p[k]; else other += p[k] })
        if (other > 0) out["other"] = other
        return out
    }
    // GPU memory by owner; the remainder is render targets, shadow maps, pools.
    readonly property var memoryModel: {
        if (!has("gpuMemoryUsed")) return null
        const parts = {
            "textures": num("textureMemory"),
            "vertex data": num("primvar"),
            "indices": num("topology"),
            "volumes": num("volumeMemory"),
            "RT structures": num("accelerationStructureMemory")
        }
        let sum = 0
        for (const k in parts) sum += parts[k]
        parts["targets, pools"] = Math.max(0, num("gpuMemoryUsed") - sum)
        return parts
    }

    width: parent.width
    height: col.height

    Column {
        id: col
        width: root.availableWidth
        spacing: 8
        leftPadding: 8
        rightPadding: 8
        bottomPadding: 8

        // Header: who, what stage, how far.
        Row {
            width: parent.width - 16
            spacing: 8
            Text {
                text: root.has("rendererName") ? root.sd.rendererName : "Eldr"
                size: Typography.large
                color: Theme.fg
            }
            Text {
                text: root.has("rendererStage") ? root.sd.rendererStage : ""
            }
        }
        Column {
            width: parent.width - 16
            spacing: 4

            Row {
                width: parent.width
                spacing: root.hSpacing

                SimpleTile {
                    width: root.halfWidth
                    label: "Elapsed"
                    Duration {
                        seconds: root.elapsed
                        decimalPlaces: 0
                        showZeros: true
                        hms: false
                        color: Theme.fg
                        size: Typography.xlarge
                    }
                }
                SimpleTile {
                    width: root.halfWidth - 24
                    label: "ETA"
                    Duration {
                        seconds: root.running ? root.elapsed / root.fractionDone - root.elapsed : 0.0
                        decimalPlaces: 0
                        showZeros: true
                        hms: false
                        color: Theme.fg
                        size: Typography.xlarge
                    }
                }
            }
            ProgressBar {
                width: parent.width
                value: root.fractionDone
            }
            Text {
                width: parent.width
                visible: text.length > 0
                wrapMode: Text.Wrap
                text: root.has("renderProgressAnnotation") ? root.sd.renderProgressAnnotation : ""
            }
        }
        ItemGrid {
            width: parent.width - 16
            cellWidth: 90
            cellHeight: 40

            SimpleTile {
                label: "Samples"
                visible: root.has("numCompletedSamples")
                Text {
                    text: root.num("numCompletedSamples") + " / " + root.num("taaTarget")
                    size: Typography.large
                    color: Theme.fg
                }
            }
            SimpleTile {
                label: "Converged"
                visible: root.has("pixelsConvergedPercent")
                Text {
                    text: root.num("pixelsConvergedPercent").toFixed(0) + "%"
                    size: Typography.large
                    color: Theme.fg
                }
            }
            SimpleTile {
                label: "Resolution"
                visible: root.has("resolution")
                Text {
                    text: root.has("resolution") ? root.sd.resolution[0] + " × " + root.sd.resolution[1] : ""
                    size: Typography.large
                    color: Theme.fg
                }
            }
            SimpleTile {
                label: "Exposure"
                visible: root.has("exposureEV")
                Text {
                    text: (root.num("exposureEV") >= 0 ? "+" : "") + root.num("exposureEV").toFixed(2) + " EV"
                    size: Typography.large
                    color: Theme.fg
                }
            }
            SimpleTile {
                label: "Frame sharing"
                visible: root.has("frameSharing")
                Text {
                    text: root.has("frameSharing") ? root.sd.frameSharing : ""
                    size: Typography.large
                    color: Theme.fg
                }
            }
            SimpleTile {
                label: "Ray tracing"
                visible: root.has("rayTracingSupported")
                Text {
                    text: root.sd.rayTracingSupported ? "yes" : "no"
                    size: Typography.large
                    color: Theme.fg
                }
            }
        }

        // Memory. totalMemory = GPU + host-visible; peakMemory = the GPU peak
        // (graphics memory), not process RAM.
        ExpandingTile {
            width: parent.width - 16
            label: "Memory (GPU)"
            visible: root.has("gpuMemoryUsed") || root.has("totalMemory")
            initiallyExpanded: true

            ItemGrid {
                width: parent.width
                cellWidth: 90
                cellHeight: 40

                SimpleTile {
                    label: "GPU used"
                    visible: root.has("gpuMemoryUsed")
                    BriefNumber { number: root.num("gpuMemoryUsed"); memory: true; color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "GPU peak"
                    visible: root.has("peakMemory")
                    BriefNumber { number: root.num("peakMemory"); memory: true; color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "Host-visible"
                    visible: root.has("hostMemoryUsed")
                    BriefNumber { number: root.num("hostMemoryUsed"); memory: true; color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "GPU + host"
                    visible: root.has("totalMemory")
                    BriefNumber { number: root.num("totalMemory"); memory: true; color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "VRAM in use"
                    visible: root.num("vramUsage") > 0
                    BriefNumber { number: root.num("vramUsage"); memory: true; color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "VRAM budget"
                    visible: root.num("vramBudget") > 0
                    BriefNumber { number: root.num("vramBudget"); memory: true; color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "VRAM size"
                    visible: root.num("vramSize") > 0
                    BriefNumber { number: root.num("vramSize"); memory: true; color: Theme.fg; size: Typography.large }
                }
            }
            ProgressBar {
                width: parent.width
                visible: root.num("vramBudget") > 0
                value: root.num("vramBudget") > 0 ? Math.min(1, root.num("vramUsage") / root.num("vramBudget")) : 0
            }
            Text {
                width: parent.width
                visible: root.num("vramBudget") > 0
                text: "VRAM in use of the budget: " + root.pct(root.num("vramUsage"), root.num("vramBudget"))
                wrapMode: Text.Wrap
            }
        }
        StatTable {
            width: parent.width - 16
            label: "GPU memory by owner"
            memory: true
            showZeros: false
            interactive: true
            values: root.memoryModel
        }

        // Time.
        ItemGrid {
            width: parent.width - 16
            cellWidth: 90
            cellHeight: 40

            SimpleTile {
                label: "GPU frame"
                visible: root.has("gpuTimeMs")
                Text { text: root.ms(root.num("gpuTimeMs")); size: Typography.large; color: Theme.fg }
            }
            SimpleTile {
                label: "Render call"
                visible: root.has("renderTimeMs")
                Text { text: root.ms(root.num("renderTimeMs")); size: Typography.large; color: Theme.fg }
            }
            SimpleTile {
                label: "Scene upload"
                visible: root.has("commitTimeMs")
                Text { text: root.ms(root.num("commitTimeMs")); size: Typography.large; color: Theme.fg }
            }
        }
        Text {
            width: parent.width - 16
            visible: root.has("loadLine") && root.sd.loadLine.length > 0
            text: root.has("loadLine") ? root.sd.loadLine : ""
            wrapMode: Text.Wrap
        }
        StatTable {
            width: parent.width - 16
            label: "GPU time by pass"
            interactive: true
            sorted: true
            formatFunction: (v) => root.ms(v)
            colorMap: ({ "other": Theme.gray })
            values: root.passModel
        }

        // Scene.
        ExpandingTile {
            width: parent.width - 16
            label: "Scene"
            visible: root.has("meshes") || root.has("trianglesTotal")
            initiallyExpanded: true

            ItemGrid {
                width: parent.width
                cellWidth: 90
                cellHeight: 40

                SimpleTile {
                    label: "Meshes"
                    visible: root.has("meshes")
                    BriefNumber { number: root.num("meshes"); color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "Volumes"
                    visible: root.has("volumes")
                    BriefNumber { number: root.num("volumes"); color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "Draw calls"
                    visible: root.has("drawCalls")
                    BriefNumber { number: root.num("drawCalls"); color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "Lights"
                    visible: root.has("lights")
                    BriefNumber { number: root.num("lights"); color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "Instances drawn"
                    visible: root.has("instancesDrawn")
                    BriefNumber { number: root.num("instancesDrawn"); color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "Instances total"
                    visible: root.has("instancesTotal")
                    BriefNumber { number: root.num("instancesTotal"); color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "Triangles drawn"
                    visible: root.has("trianglesDrawn")
                    BriefNumber { number: root.num("trianglesDrawn"); color: Theme.fg; size: Typography.large }
                }
                SimpleTile {
                    label: "Triangles total"
                    visible: root.has("trianglesTotal")
                    BriefNumber { number: root.num("trianglesTotal"); color: Theme.fg; size: Typography.large }
                }
            }
            Text {
                width: parent.width
                visible: root.num("trianglesTotal") > 0
                text: "Drawn after culling: " + root.pct(root.num("trianglesDrawn"), root.num("trianglesTotal")) + " of triangles, "
                      + root.pct(root.num("instancesDrawn"), root.num("instancesTotal")) + " of instances"
                wrapMode: Text.Wrap
            }
        }

        // Materials Eldr could not compile whole.
        Rectangle {
            width: parent.width - 16
            height: warn.height + 12
            visible: root.fallbacks > 0
            radius: 4
            color: "transparent"
            border.color: Theme.orange
            border.width: 1

            Text {
                id: warn
                x: 6; y: 6
                width: parent.width - 12
                wrapMode: Text.Wrap
                color: Theme.orange
                text: root.fallbacks + " material input(s) fell back (bypassed or unsupported nodes): "
                      + (root.has("materialFallbackNodes") ? root.sd.materialFallbackNodes : "")
            }
        }
    }
}
