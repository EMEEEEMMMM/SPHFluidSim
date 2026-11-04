import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import IISPH.Theme
import IISPH.View

Flickable {
    id: panel
    property var session
    signal sceneRequested(int scene)
    signal resolutionRequested(int index)

    clip: true
    contentWidth: width
    contentHeight: Math.max(height, column.implicitHeight)
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property var p: session ? session.params : ({})
    readonly property var st: session ? session.status : ({})

    readonly property var resolutions: [
        "Low · 0.040 m", "Medium · 0.030 m", "High · 0.025 m", "Ultra · 0.020 m",
        "Cinema · 0.015 m", "Epic · 0.0125 m", "Extreme · 0.010 m"
    ]
    readonly property var spacings: [0.040, 0.030, 0.025, 0.020, 0.015, 0.0125, 0.010]

    readonly property var tracePoints: {
        const src = session ? session.densityTrace : [];
        const rho0 = panel.st.rho0 !== undefined ? panel.st.rho0 : 1000;
        const out = [];
        for (let i = 0; i < src.length; ++i) {
            const rho = src[i].density !== undefined
                      ? src[i].density
                      : rho0 * (1 + src[i].e);
            if (rho > rho0 * 0.5 && rho < rho0 * 1.5)
                out.push({ x: src[i].t, y: rho, e: src[i].e, step: src[i].step });
        }
        return out;
    }

    // Error bands (fractions): a converged run sits near 0.010 %, so green
    // covers an order of magnitude around it before amber and red kick in.
    readonly property real errGood: 0.001
    readonly property real errWarn: 0.010

    function errColor(error) {
        const e = Math.abs(error);
        if (e >= errWarn) return c.danger;
        if (e >= errGood) return c.warn;
        return c.ok;
    }

    readonly property real densityError: panel.st.densityError !== undefined
                                        ? panel.st.densityError : 0

    function resolutionIndex() {
        const s = p.spacing !== undefined ? p.spacing : 0.025;
        for (let i = 0; i < spacings.length; ++i)
            if (Math.abs(s - spacings[i]) < 1e-4) return i;
        return 2;
    }

    function groupDigits(value) {
        const raw = String(Math.round(value));
        let out = "";
        let count = 0;
        for (let i = raw.length - 1; i >= 0; --i) {
            out = raw[i] + out;
            if (++count % 3 === 0 && i > 0) out = " " + out;
        }
        return out;
    }

    ColumnLayout {
        id: column
        width: panel.width
        height: Math.max(panel.height, implicitHeight)
        spacing: panel.m.cardGap

        Card {
            title: "Scene"
            badge: "open tank"
            Layout.fillWidth: true

            Segmented {
                Layout.fillWidth: true
                options: ["Dam break", "Inflow"]
                currentIndex: panel.p.scene !== undefined ? panel.p.scene : 0
                onActivated: (index) => panel.sceneRequested(index)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    text: "Resolution"
                    color: panel.c.fg2
                    font.pixelSize: 11
                }
                Item { Layout.fillWidth: true }

                ComboBox {
                    id: resCombo
                    implicitWidth: 136
                    model: panel.resolutions
                    currentIndex: panel.resolutionIndex()
                    onActivated: (index) => panel.resolutionRequested(index)

                    background: Rectangle {
                        radius: panel.c && Theme.metrics ? Theme.metrics.radiusSm : 6
                        color: panel.c.field
                        border.width: 1
                        border.color: panel.c.border
                    }
                    contentItem: Text {
                        leftPadding: 8
                        rightPadding: 18
                        text: resCombo.displayText
                        color: panel.c.fg
                        font.pixelSize: 11
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    indicator: IconItem {
                        x: resCombo.width - width - 8
                        y: (resCombo.height - height) / 2
                        width: 10
                        height: 10
                        kind: 8
                        angle: 90
                        color: panel.c.fg2
                    }
                    delegate: ItemDelegate {
                        width: resCombo.width
                        contentItem: Text {
                            text: modelData
                            color: highlighted ? "#FFFFFF" : panel.c.fg
                            font.pixelSize: 11
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: highlighted ? panel.c.accent : panel.c.card
                        }
                    }
                    popup: Popup {
                        y: resCombo.height + 4
                        width: resCombo.width
                        implicitHeight: contentItem.implicitHeight + 2
                        padding: 1
                        background: Rectangle {
                            radius: Theme.metrics.radiusSm
                            color: panel.c.card
                            border.width: 1
                            border.color: panel.c.border
                        }
                        contentItem: ListView {
                            clip: true
                            implicitHeight: contentHeight
                            model: resCombo.popup.visible ? resCombo.delegateModel : null
                            currentIndex: resCombo.highlightedIndex
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "Particles"
                    color: panel.c.fg2
                    font.pixelSize: 11
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: panel.groupDigits(panel.st.fluidCount !== undefined ? panel.st.fluidCount : 0)
                    color: panel.c.fg
                    font.pixelSize: 11
                }
            }

            Rectangle {
                Layout.fillWidth: true
                visible: (panel.p.scene !== undefined ? panel.p.scene : 0) === 0
                implicitHeight: panel.m.controlH
                radius: panel.m.square ? 0 : panel.m.radiusSm
                color: dropArea.containsMouse && !panel.st.hasDrop
                       ? panel.c.chip
                       : (panel.m.square ? "transparent" : panel.c.field)
                border.width: 1
                border.color: panel.m.square ? panel.c.borderStrong : panel.c.border

                Row {
                    anchors.centerIn: parent
                    spacing: 6

                    IconItem {
                        width: 11
                        height: 11
                        kind: 9
                        anchors.verticalCenter: parent.verticalCenter
                        color: panel.st.hasDrop ? panel.c.fg3 : panel.c.fg
                    }
                    Text {
                        text: panel.st.hasDrop ? "Drop spawned" : "Spawn water drop"
                        color: panel.st.hasDrop ? panel.c.fg3 : panel.c.fg
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                }
                MouseArea {
                    id: dropArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !panel.st.hasDrop
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.session.spawnDrop()
                }
            }
        }

        Card {
            title: "Playback"
            badge: panel.session && panel.session.running ? "running" : "paused"
            badgeLive: panel.session && panel.session.running
            Layout.fillWidth: true

            SliderRow {
                Layout.fillWidth: true
                label: "Speed"
                from: 1
                to: 240
                decimals: 0
                suffix: " steps/s"
                value: panel.p.speed !== undefined ? panel.p.speed : 60
                onEdited: (v) => panel.session.setParam("speed", Math.round(v))
            }

            RowLayout {
                Layout.fillWidth: true
                Text { text: "Measured step rate"; color: panel.c.fg2; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Text {
                    text: panel.st.msPerStep > 0.01
                          ? Math.round(1000 / panel.st.msPerStep) + " steps/s" : "—"
                    color: panel.c.fg
                    font.pixelSize: 11
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Text { text: "Step cost"; color: panel.c.fg2; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Text {
                    text: panel.st.msPerStep !== undefined
                          ? Number(panel.st.msPerStep).toFixed(1) + " ms" : "—"
                    color: panel.c.fg
                    font.pixelSize: 11
                }
            }
        }

        Card {
            title: "Metrics"
            badge: "live"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 300

            MetricGrid {
                Layout.fillWidth: true
                keys: ["Sim time", "Density err", "Pressure iters", "Triangles"]
                values: [
                    Number(panel.st.simTime !== undefined ? panel.st.simTime : 0).toFixed(2) + " s",
                    (Math.abs(panel.densityError) * 100).toFixed(3) + " %",
                    String(panel.st.pressureIterations !== undefined ? panel.st.pressureIterations : 0),
                    panel.groupDigits(panel.session ? panel.session.triangleCount : 0)
                ]
                colors: [panel.c.fg, panel.errColor(panel.densityError), panel.c.fg, panel.c.fg]
            }

            RowLayout {
                Layout.fillWidth: true
                Text { text: "Mean density"; color: panel.c.fg2; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Text {
                    text: Number(panel.st.meanDensity !== undefined ? panel.st.meanDensity : 1000).toFixed(2)
                          + " kg/m³"
                    color: panel.c.fg
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    font.family: panel.m.numberFamily
                }
            }

            Spark {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: 120
                points: panel.tracePoints
                referenceDensity: panel.st.rho0 !== undefined ? panel.st.rho0 : 1000
            }

            Row {
                Layout.fillWidth: true
                spacing: 10

                Text {
                    text: "err"
                    color: panel.c.fg3
                    font.pixelSize: 9
                    font.family: panel.m.numberFamily
                }
                Row {
                    spacing: 4
                    Text {
                        text: "●"
                        color: panel.c.ok
                        font.pixelSize: 9
                    }
                    Text {
                        text: "≤0.1 %"
                        color: panel.c.fg3
                        font.pixelSize: 9
                        font.family: panel.m.numberFamily
                    }
                }
                Row {
                    spacing: 4
                    Text {
                        text: "●"
                        color: panel.c.warn
                        font.pixelSize: 9
                    }
                    Text {
                        text: "0.1-1 %"
                        color: panel.c.fg3
                        font.pixelSize: 9
                        font.family: panel.m.numberFamily
                    }
                }
                Row {
                    spacing: 4
                    Text {
                        text: "●"
                        color: panel.c.danger
                        font.pixelSize: 9
                    }
                    Text {
                        text: "≥1 %"
                        color: panel.c.fg3
                        font.pixelSize: 9
                        font.family: panel.m.numberFamily
                    }
                }
            }
        }
    }
}
