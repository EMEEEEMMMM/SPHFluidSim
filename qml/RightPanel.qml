import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import IISPH.Theme

Flickable {
    id: panel
    property var session

    signal solverRequested(int solver)
    // Property-change signal name is taken by `cameraMode`, so the user
    // action gets its own name (matches sceneRequested / resolutionRequested).
    signal cameraModeRequested(int mode)
    signal resetViewRequested()

    clip: true
    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property var p: session ? session.params : ({})
    property bool advancedOpen: false
    property bool perfHud: true
    property bool sceneHud: true
    property real fieldOfView: 45
    property int cameraMode: 0

    ColumnLayout {
        id: column
        width: panel.width
        spacing: panel.m.cardGap

        Card {
            title: "Solver"
            badge: (panel.p.solver !== undefined && panel.p.solver === 1) ? "DFSPH" : "IISPH"
            Layout.fillWidth: true

            Segmented {
                Layout.fillWidth: true
                options: ["IISPH", "DFSPH"]
                currentIndex: panel.p.solver !== undefined ? panel.p.solver : 0
                onActivated: (index) => panel.solverRequested(index)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                IconBtn {
                    kind: 8
                    implicitWidth: 18
                    implicitHeight: 18
                    iconRatio: 0.7
                    active: panel.advancedOpen
                    onClicked: panel.advancedOpen = !panel.advancedOpen
                }
                Text {
                    text: "Advanced"
                    color: panel.c.fg2
                    font.pixelSize: 11
                }
                Item { Layout.fillWidth: true }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.advancedOpen
                spacing: 7

                SliderRow {
                    Layout.fillWidth: true
                    label: "omega"
                    from: 0.1; to: 0.9; decimals: 3
                    value: panel.p.omega !== undefined ? panel.p.omega : 0.5
                    onEdited: (v) => panel.session.setParam("omega", v)
                }
                SliderRow {
                    Layout.fillWidth: true
                    visible: (panel.p.solver !== undefined ? panel.p.solver : 0) === 0
                    label: "eta"
                    from: 0.0001; to: 0.01; decimals: 4
                    value: panel.p.eta !== undefined ? panel.p.eta : 0.0001
                    onEdited: (v) => panel.session.setParam("eta", v)
                }
                SliderRow {
                    Layout.fillWidth: true
                    visible: (panel.p.solver !== undefined ? panel.p.solver : 0) === 1
                    label: "eta_v"
                    from: 0.0001; to: 0.01; decimals: 4
                    value: panel.p.etaV !== undefined ? panel.p.etaV : 0.001
                    onEdited: (v) => panel.session.setParam("etaV", v)
                }
                SliderRow {
                    Layout.fillWidth: true
                    label: "Max iterations"
                    from: 2; to: 100; decimals: 0
                    value: panel.p.maxIter !== undefined ? panel.p.maxIter : 100
                    onEdited: (v) => panel.session.setParam("maxIter", Math.round(v))
                }
                SliderRow {
                    Layout.fillWidth: true
                    label: "dt"
                    from: 0.0005; to: 0.01; decimals: 4; suffix: " s"
                    value: panel.p.dt !== undefined ? panel.p.dt : 0.003
                    onEdited: (v) => panel.session.setParam("dt", v)
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "CFL step limiter"; color: panel.c.fg2; font.pixelSize: 11 }
                    Item { Layout.fillWidth: true }
                    Toggle {
                        checked: panel.p.cfl !== undefined ? panel.p.cfl : true
                        onToggled: (on) => panel.session.setParam("cfl", on)
                    }
                }
            }
        }

        Card {
            title: "Forces"
            badge: "Sec. 5"
            Layout.fillWidth: true

            SliderRow {
                Layout.fillWidth: true
                label: "XSPH viscosity"
                from: 0; to: 1; decimals: 3
                value: panel.p.viscosity !== undefined ? panel.p.viscosity : 0.1
                onEdited: (v) => panel.session.setParam("viscosity", v)
            }
            SliderRow {
                Layout.fillWidth: true
                label: "Surface tension"
                from: 0; to: 0.5; decimals: 3
                value: panel.p.surfaceTension !== undefined ? panel.p.surfaceTension : 0.05
                onEdited: (v) => panel.session.setParam("surfaceTension", v)
            }
            SliderRow {
                Layout.fillWidth: true
                label: "Wall adhesion"
                from: 0; to: 0.1; decimals: 3
                value: panel.p.wallAdhesion !== undefined ? panel.p.wallAdhesion : 0.01
                onEdited: (v) => panel.session.setParam("wallAdhesion", v)
            }
        }

        Card {
            title: "Surface"
            badge: "Akinci 2012"
            Layout.fillWidth: true

            SliderRow {
                Layout.fillWidth: true
                label: "Threshold"
                from: 0.3; to: 0.95; decimals: 2
                value: panel.p.surfaceThreshold !== undefined ? panel.p.surfaceThreshold : 0.8
                onEdited: (v) => panel.session.setParam("surfaceThreshold", v)
            }
            SliderRow {
                Layout.fillWidth: true
                label: "Grid scale"
                from: 0.5; to: 2.0; decimals: 1; suffix: " × r"
                value: panel.p.gridScale !== undefined ? panel.p.gridScale : 0.5
                onEdited: (v) => panel.session.setParam("gridScale", v)
            }
            SliderRow {
                Layout.fillWidth: true
                label: "Rebuild every"
                from: 1; to: 20; decimals: 0; suffix: " frames"
                value: panel.p.reconInterval !== undefined ? panel.p.reconInterval : 3
                onEdited: (v) => panel.session.setParam("reconInterval", Math.round(v))
            }
        }

        Card {
            title: "Camera"
            badge: panel.cameraMode === 0 ? "orbit" : "fly"
            Layout.fillWidth: true

            Segmented {
                Layout.fillWidth: true
                options: ["Orbit", "Fly"]
                currentIndex: panel.cameraMode
                onActivated: (index) => panel.cameraModeRequested(index)
            }

            SliderRow {
                id: fovRow
                Layout.fillWidth: true
                label: "Field of view"
                from: 20; to: 75; decimals: 0; suffix: "°"
                value: panel.fieldOfView
                onEdited: (v) => panel.fovEdited(v)
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: panel.m.controlH
                radius: panel.m.square ? 0 : panel.m.radiusSm
                color: resetArea.containsMouse ? panel.c.chip : "transparent"
                border.width: 1
                border.color: panel.c.border
                Text {
                    anchors.centerIn: parent
                    text: "Reset view"
                    color: panel.c.fg2
                    font.pixelSize: 11
                    font.weight: Font.Medium
                }
                MouseArea {
                    id: resetArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.resetViewRequested()
                }
            }
        }

        Card {
            title: "Overlay"
            Layout.fillWidth: true

            RowLayout {
                Layout.fillWidth: true
                Text { text: "Performance HUD"; color: panel.c.fg2; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Toggle { checked: panel.perfHud; onToggled: (on) => panel.perfHud = on }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "Scene label"; color: panel.c.fg2; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Toggle { checked: panel.sceneHud; onToggled: (on) => panel.sceneHud = on }
            }
        }
    }

    function setFieldOfView(value) {
        fovRow.value = value;
    }

    signal fovEdited(real value)
}
