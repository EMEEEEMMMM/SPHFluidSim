import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

Rectangle {
    id: nav
    property var session

    signal screenshotRequested()
    signal renderRequested()

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitHeight: 32
    color: c.panel

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: nav.c.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 12
        spacing: 2

        NavMenuButton {
            id: fileMenuButton
            text: qsTr("File")
            menuWidth: 246

            NavAction {
                text: qsTr("Import 3D Model ...")
                onSelectedAction: {
                    fileMenuButton.closeMenu();
                    if (nav.session) nav.session.importModel();
                }
            }
            NavAction {
                text: qsTr("Import Parameter (Json) ...")
                shortcut: "Ctrl+Shift+O"
                onSelectedAction: {
                    fileMenuButton.closeMenu();
                    if (nav.session) nav.session.importParameters();
                }
            }
            NavAction {
                text: qsTr("Export Parameter (Json) ...")
                shortcut: "Ctrl+Shift+S"
                onSelectedAction: {
                    fileMenuButton.closeMenu();
                    if (nav.session) nav.session.exportParameters();
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                height: 1
                color: nav.c.border
            }
            NavAction {
                text: qsTr("Restart scene")
                shortcut: "R"
                onSelectedAction: {
                    fileMenuButton.closeMenu();
                    if (nav.session) nav.session.resetSimulation();
                }
            }
            NavAction {
                text: qsTr("Save snapshot")
                shortcut: "Ctrl+S"
                onSelectedAction: {
                    fileMenuButton.closeMenu();
                    nav.screenshotRequested();
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                height: 1
                color: nav.c.border
            }

            NavAction {
                text: qsTr("Exit")
                shortcut: "Ctrl+Q"
                danger: true
                onSelectedAction: Qt.quit()
            }
        }

        Button {
            id: renderButton
            text: qsTr("Render")
            implicitWidth: renderLabel.implicitWidth + 24
            implicitHeight: 24
            padding: 0
            hoverEnabled: true
            Accessible.name: qsTr("Open render settings")
            onClicked: nav.renderRequested()

            background: Rectangle {
                radius: nav.m.square ? 0 : nav.m.radiusSm
                color: renderButton.hovered ? nav.c.chip : "transparent"
            }

            contentItem: Text {
                id: renderLabel
                text: renderButton.text
                color: renderButton.hovered ? nav.c.fg : nav.c.fg2
                font.pixelSize: 11
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        NavMenuButton {
            id: appearanceMenuButton
            text: qsTr("Appearance")
            menuWidth: 286

            Repeater {
                model: Theme.themes

                NavAction {
                    required property var modelData

                    text: modelData.name
                    subtitle: modelData.blurb
                    themeIndex: modelData.index
                    selected: Theme.index === modelData.index
                    onSelectedAction: {
                        Theme.index = modelData.index;
                        appearanceMenuButton.closeMenu();
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                height: 1
                color: nav.c.border
            }

            NavAction {
                text: qsTr("Next theme")
                shortcut: "Ctrl+T"
                onSelectedAction: {
                    appearanceMenuButton.closeMenu();
                    Theme.cycle();
                }
            }
        }

        NavMenuButton {
            id: helpMenuButton
            text: qsTr("Help")
            menuWidth: 230

            NavAction {
                text: qsTr("Keyboard shortcuts")
                onSelectedAction: {
                    helpMenuButton.closeMenu();
                    shortcutsDialog.open();
                }
            }
            NavAction {
                text: qsTr("About SPHFluidSim")
                onSelectedAction: {
                    helpMenuButton.closeMenu();
                    aboutDialog.open();
                }
            }
        }

        Item { Layout.fillWidth: true }

        Text {
            text: Theme.name
            color: nav.c.fg3
            font.pixelSize: 9
            font.family: nav.m.numberFamily
        }
    }

    InfoDialog {
        id: shortcutsDialog
        title: qsTr("Keyboard shortcuts")
        bodyText: "<table cellspacing='3'>"
                  + "<tr><td><b>Space</b></td><td>Pause / resume</td></tr>"
                  + "<tr><td><b>S</b></td><td>Single step (Orbit) · move back (Fly)</td></tr>"
                  + "<tr><td><b>R</b></td><td>Restart the scene</td></tr>"
                  + "<tr><td><b>Ctrl+T</b></td><td>Cycle appearance</td></tr>"
                  + "<tr><td><b>Ctrl+S</b></td><td>Save screenshot</td></tr>"
                  + "<tr><td><b>Ctrl+Shift+O</b></td><td>Import parameters (JSON)</td></tr>"
                  + "<tr><td><b>Ctrl+Shift+S</b></td><td>Export parameters (JSON)</td></tr>"
                  + "<tr><td><b>Ctrl+Shift+R</b></td><td>Open render settings</td></tr>"
                  + "<tr><td><b>F11</b></td><td>Toggle fullscreen</td></tr>"
                  + "<tr><td><b>1 / 2 / 3</b></td><td>Particles / surface / both</td></tr>"
                  + "</table><br>"
                  + "Camera has two modes: Orbit (left-drag orbits) and Fly (left-drag looks, WASD moves, Q/E vertical). "
                  + "Middle/right-drag pans; the wheel changes the field of view."
    }

    InfoDialog {
        id: aboutDialog
        title: qsTr("SPHFluidSim")
        bodyText: "<b>Real-time implicit incompressible SPH fluid simulation.</b>"
                  + "<br><br>IISPH pressure solve (Ihmsen et al., TVCG 2013) with a DFSPH "
                  + "alternative (Bender &amp; Koschier, 2017) and Akinci-style surface "
                  + "reconstruction. Qt Widgets and Qt Quick frontends, OpenGL 3.3 renderer."
        linkText: "github.com/EMEEEEMMMM/SPHFluidSim"
        linkUrl: "https://github.com/EMEEEEMMMM/SPHFluidSim"
    }
}
