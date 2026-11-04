import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls
import IISPH.Theme
import IISPH.View

Window {
    id: root

    visible: true
    width: Math.min(1580, Screen.desktopAvailableWidth - 80)
    height: Math.min(950, Screen.desktopAvailableHeight - 90)
    minimumWidth: 1120
    minimumHeight: 700
    title: "SPHFluidSim — Implicit Incompressible SPH"
    flags: Qt.Window | Qt.FramelessWindowHint
    color: Theme.colors.bg

    readonly property var sim: session
    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    property real fpsValue: 0
    property string message: ""
    // 0 = Orbit, 1 = Fly. Drives the camera card and the S shortcut: in Fly
    // mode S is "move backwards" and must not trigger a single step.
    property int cameraMode: 0

    FrameAnimation {
        running: true
        onTriggered: {
            const inst = frameTime > 0 ? 1 / frameTime : 0;
            root.fpsValue = root.fpsValue <= 0 ? inst : root.fpsValue * 0.9 + inst * 0.1;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TitleBar {
            Layout.fillWidth: true
            session: root.sim
            onViewModeRequested: (mode) => root.sim.setParam("viewMode", mode)
            onScreenshotRequested: root.saveScreenshot()
            onFullscreenRequested: root.toggleFullscreen()
        }

        NavBar {
            Layout.fillWidth: true
            session: root.sim
            onScreenshotRequested: root.saveScreenshot()
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: root.m.cardGap

                LeftPanel {
                    Layout.preferredWidth: 272
                    Layout.fillHeight: true
                    session: root.sim
                    onSceneRequested: (scene) => root.requestScene(scene)
                    onResolutionRequested: (index) => root.requestResolution(index)
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ViewportItem {
                        id: viewportProbe
                        anchors.fill: parent
                        session: root.sim
                        viewMode: root.sim.params.viewMode !== undefined ? root.sim.params.viewMode : 0
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: 1
                        border.color: root.c.border
                        radius: root.m.square ? 0 : root.m.radiusLg
                    }

                    Pill {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 12
                        text: root.hudText()
                    }
                }

                RightPanel {
                    id: rightPanel
                    Layout.preferredWidth: 300
                    Layout.fillHeight: true
                    session: root.sim
                    fieldOfView: viewportProbe.fieldOfView
                    cameraMode: root.cameraMode
                    onSolverRequested: (solver) => root.requestSolver(solver)
                    onCameraModeRequested: (mode) => {
                        root.cameraMode = mode;
                        viewportProbe.orbitOnly = mode === 0;
                        viewportProbe.forceActiveFocus();
                    }
                    onResetViewRequested: viewportProbe.resetCamera()
                    onFovEdited: (value) => viewportProbe.fieldOfView = value
                }
            }
        }

        StatusBar {
            Layout.fillWidth: true
            session: root.sim
            leftText: root.message !== ""
                      ? root.message
                      : "OpenGL 3.3 core · Qt Quick · " + (root.sim.params.scene === 1 ? "inflow" : "dam break")
        }
    }

    Popup {
        id: confirmPopup
        anchors.centerIn: parent
        modal: true
        focus: true
        width: 360
        padding: 0

        property string text: ""
        property var onAccept: null

        background: Rectangle {
            radius: root.m.radiusLg
            color: root.c.card
            border.width: 1
            border.color: root.c.border
        }

        contentItem: ColumnLayout {
            spacing: 12
            Layout.margins: 0

            Text {
                Layout.fillWidth: true
                Layout.margins: 16
                text: confirmPopup.text
                color: root.c.fg
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 14
                spacing: 8
                Item { Layout.fillWidth: true }

                Rectangle {
                    implicitWidth: 84
                    implicitHeight: 28
                    radius: root.m.radiusSm
                    color: root.c.field
                    border.width: 1
                    border.color: root.c.border
                    Text {
                        anchors.centerIn: parent
                        text: "Cancel"
                        color: root.c.fg
                        font.pixelSize: 11
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: confirmPopup.close()
                    }
                }

                Rectangle {
                    implicitWidth: 92
                    implicitHeight: 28
                    radius: root.m.radiusSm
                    color: root.c.accent
                    Text {
                        anchors.centerIn: parent
                        text: "Continue"
                        color: "#FFFFFF"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const action = confirmPopup.onAccept;
                            confirmPopup.close();
                            if (action) action();
                        }
                    }
                }
            }
        }
    }

    Shortcut { sequence: "Space"; onActivated: root.sim.toggleRunning() }
    // Orbit only: in Fly mode S belongs to the WASD flight controls.
    Shortcut {
        sequence: "S"
        enabled: root.cameraMode === 0
        onActivated: root.sim.stepOnce()
    }
    Shortcut { sequence: "R"; onActivated: root.sim.resetSimulation() }
    Shortcut { sequence: "Ctrl+T"; onActivated: Theme.cycle() }
    Shortcut { sequence: "Ctrl+S"; onActivated: root.saveScreenshot() }
    Shortcut { sequence: "F11"; onActivated: root.toggleFullscreen() }

    function hudText() {
        const st = root.sim.status;
        const parts = [];
        if (rightPanel.perfHud) {
            parts.push(Math.round(root.fpsValue) + " fps");
            if (st.msPerStep > 0.01) parts.push(Number(st.msPerStep).toFixed(1) + " ms/step");
        }
        if (rightPanel.sceneHud) {
            parts.push(st.scene === 1 ? "open tank 1.0³ (inflow)" : "tank 2.0 × 1.2 × 1.0 m");
            parts.push("N=" + (st.fluidCount !== undefined ? st.fluidCount : 0));
            parts.push("r=" + Number(st.spacing !== undefined ? st.spacing : 0.025).toFixed(3) + " m");
        }
        return parts.join("   ·   ");
    }

    function toggleFullscreen() {
        visibility = visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen;
    }

    function requestScene(scene) {
        if (root.sim.params.scene === scene) return;
        confirmPopup.text = "Switching the scene resets the simulation. Continue?";
        confirmPopup.onAccept = function () { root.sim.setParam("scene", scene); };
        confirmPopup.open();
    }

    function requestResolution(index) {
        const spacings = [0.040, 0.030, 0.025, 0.020, 0.015, 0.0125, 0.010];
        if (Math.abs(root.sim.params.spacing - spacings[index]) < 1e-5) return;
        confirmPopup.text = "Changing the resolution rebuilds the scene from scratch. Continue?";
        confirmPopup.onAccept = function () { root.sim.setParam("spacing", spacings[index]); };
        confirmPopup.open();
    }

    function requestSolver(solver) {
        if (root.sim.params.solver === solver) return;
        confirmPopup.text = "The solver state differs between IISPH and DFSPH, so the scene is restarted. Continue?";
        confirmPopup.onAccept = function () { root.sim.setParam("solver", solver); };
        confirmPopup.open();
    }

    function saveScreenshot() {
        const path = capture.save(root);
        root.message = path === "" ? "Screenshot failed" : "Saved " + path;
        messageTimer.restart();
    }

    Timer {
        id: messageTimer
        interval: 2600
        onTriggered: root.message = ""
    }
}
