import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import IISPH.Theme
import IISPH.View

Flickable {
    id: panel
    property var session
    signal sceneRequested(int scene)
    signal particleCountRequested(int count)

    clip: true
    contentWidth: width
    contentHeight: Math.max(height, column.implicitHeight)
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property var p: session ? session.params : ({})
    readonly property var st: session ? session.status : ({})

    readonly property int particleMin: 512
    readonly property int particleMax: 4000000
    readonly property int particleTarget: p.particleCount !== undefined ? p.particleCount : 0
    // Device memory one particle needs (0 on the CPU backend, which has none).
    readonly property real bytesPerParticle: st.bytesPerParticle !== undefined ? st.bytesPerParticle : 0
    readonly property real vramFree: st.vramFreeBytes !== undefined ? st.vramFreeBytes : 0
    readonly property real neededBytes: bytesPerParticle * particleTarget
    readonly property bool overBudget: bytesPerParticle > 0 && vramFree > 0
                                       && neededBytes > vramFree * 0.9

    function formatBytes(bytes) {
        if (!(bytes > 0)) return "—"
        if (bytes >= 1024 * 1024 * 1024) return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
        return Math.round(bytes / (1024 * 1024)) + " MB"
    }

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
                    text: "Particles"
                    color: panel.c.fg2
                    font.pixelSize: 11
                }
                Item { Layout.fillWidth: true }

                Rectangle {
                    implicitWidth: 104
                    implicitHeight: 24
                    radius: panel.m.square ? 0 : panel.m.radiusSm
                    color: panel.c.field
                    border.width: countField.activeFocus ? 2 : 1
                    border.color: countField.activeFocus ? panel.c.accent
                                : (panel.overBudget ? panel.c.danger : panel.c.border)

                    TextField {
                        id: countField
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        padding: 0
                        color: panel.overBudget ? panel.c.danger : panel.c.fg
                        selectionColor: panel.c.accent
                        selectedTextColor: panel.c.accentFg
                        font.pixelSize: 11
                        font.family: panel.m.numberFamily
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        inputMethodHints: Qt.ImhDigitsOnly
                        // Do NOT attach an IntValidator here: the display text
                        // is grouped ("32 768"), which IntValidator rejects, so
                        // Enter never produced editingFinished/accepted and the
                        // field looked dead. commit() validates and clamps.
                        Component.onCompleted: text = panel.groupDigits(panel.particleTarget)

                        property double lastCommitMs: 0

                        function commit() {
                            const now = Date.now();
                            // Enter fires both accepted and editingFinished.
                            if (now - lastCommitMs < 300) return;
                            lastCommitMs = now;
                            const parsed = parseInt(text.replace(/\s/g, ""), 10);
                            if (!isFinite(parsed)) {
                                text = panel.groupDigits(panel.particleTarget);
                                return;
                            }
                            const wanted = Math.max(panel.particleMin,
                                                    Math.min(panel.particleMax, parsed));
                            // Show the committed target again; if the user
                            // accepts the confirmation, particleTarget changes
                            // and the Connections handler writes the new count.
                            text = panel.groupDigits(panel.particleTarget);
                            if (wanted !== panel.particleTarget)
                                panel.particleCountRequested(wanted);
                        }

                        onAccepted: commit()
                        onEditingFinished: commit()
                        // Keep the field in sync when the count changes elsewhere
                        // (parameter import, backend switch, scene reset). Do not
                        // skip while focused: after a rebuild the field must show
                        // the count that was actually built.
                        Connections {
                            target: panel
                            function onParticleTargetChanged() {
                                countField.text = panel.groupDigits(panel.particleTarget);
                            }
                        }
                        background: Item {}
                    }
                }
            }

            // Spacing is derived from the count; showing it keeps the old
            // resolution presets' information without the vague labels.
            Text {
                Layout.fillWidth: true
                text: "spacing " + Number(panel.st.spacing !== undefined ? panel.st.spacing : 0).toFixed(4)
                      + " m" + (panel.bytesPerParticle > 0
                                ? "  ·  " + panel.formatBytes(panel.neededBytes)
                                  + " / " + panel.formatBytes(panel.vramFree) + " free"
                                : "")
                color: panel.overBudget ? panel.c.danger : panel.c.fg3
                font.pixelSize: 10
                font.family: panel.m.numberFamily
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                visible: panel.overBudget
                text: "More particles than the GPU can hold — lower the count or the solver will refuse to build the scene."
                color: panel.c.danger
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "Active"
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
