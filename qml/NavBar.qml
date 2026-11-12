import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import IISPH.Theme

Rectangle {
    id: nav
    property var session
    // Bound by Main.qml (`isEmpty: root.isEmpty`). Must exist or the whole
    // window fails to load with "Cannot assign to non-existent property".
    property bool isEmpty: false

    signal screenshotRequested()
    signal renderRequested()
    // Scene switches must go through Main.qml's confirmPopup. Main.qml binds
    // onSceneChangeRequested -> root.requestSceneIndex(index) -> setScene().
    // The argument is the session sceneIndex (0 = dam, 1 = inflow, 2 = empty).
    signal sceneChangeRequested(int index)

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    // QColor -> "#RRGGBB" for session.setViewportBackgroundHex().
    function colorToHex(col) {
        if (!col || col.r === undefined) return "#1F2638";
        function byte(v) {
            const n = Math.max(0, Math.min(255, Math.round(Number(v) * 255)));
            return (n < 16 ? "0" : "") + n.toString(16).toUpperCase();
        }
        return "#" + byte(col.r) + byte(col.g) + byte(col.b);
    }

    // Qt6 QtQuick.Dialogs returns QUrls; session.importModels() wants local
    // paths. Mirrors ObjectsPanel.qml's conversion.
    function toLocalPath(u) {
        let s = String(u);
        if (s.indexOf("file:///") === 0) s = s.substring(8);
        else if (s.indexOf("file:/") === 0) s = s.substring(6);
        try { s = decodeURIComponent(s); } catch (e) {}
        if (s.length > 1 && s[1] !== ":" && s[0] !== "/")
            s = "/" + s;
        return s;
    }

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
                // Models belong to the Empty scene only; the fluid scenes keep
                // their tank/particles and ignore imported geometry.
                enabled: nav.isEmpty
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
            enabled: !nav.isEmpty
            Accessible.name: qsTr("Open render settings")
            onClicked: nav.renderRequested()

            background: Rectangle {
                radius: nav.m.square ? 0 : nav.m.radiusSm
                color: renderButton.enabled && renderButton.hovered
                       ? nav.c.chip : "transparent"
            }

            contentItem: Text {
                id: renderLabel
                text: renderButton.text
                color: !renderButton.enabled
                       ? nav.c.fg3
                       : (renderButton.hovered ? nav.c.fg : nav.c.fg2)
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
            id: optionMenuButton
            text: qsTr("Option")
            menuWidth: 246

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 6
                Layout.topMargin: 4
                text: qsTr("SCENE")
                color: nav.c.fg3
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
            }

            // Visual order: 0 = Empty, 1 = Dam break, 2 = Inflow.
            // Map to session sceneIndex: empty = 2, dam = 0, inflow = 1.
            Repeater {
                model: [
                    { name: qsTr("Empty"),     scene: 2 },
                    { name: qsTr("Dam break"), scene: 0 },
                    { name: qsTr("Inflow"),    scene: 1 }
                ]

                NavAction {
                    required property var modelData
                    text: modelData.name
                    showCheck: true
                    selected: nav.session && nav.session.sceneIndex === modelData.scene
                    onSelectedAction: {
                        optionMenuButton.closeMenu();
                        nav.sceneChangeRequested(modelData.scene);
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
                text: qsTr("Background color...")
                onSelectedAction: {
                    optionMenuButton.closeMenu();
                    backgroundColorDialog.open();
                }
            }

            NavAction {
                text: qsTr("Import 3D Model ...")
                // Multi-select import is only meaningful in the Empty scene.
                enabled: nav.isEmpty
                onSelectedAction: {
                    optionMenuButton.closeMenu();
                    importModelsDialog.open();
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

    // Qt6 QtQuick.Dialogs ColorDialog: selectedColor is the picked QColor.
    ColorDialog {
        id: backgroundColorDialog
        title: qsTr("Viewport background")
        selectedColor: Theme.viewportBackground !== undefined
                       ? Theme.viewportBackground : "#1F2638"
        onAccepted: {
            const col = backgroundColorDialog.selectedColor;
            if (Theme.viewportBackground !== undefined)
                Theme.viewportBackground = col;
            if (nav.session && nav.session.setViewportBackgroundHex !== undefined)
                nav.session.setViewportBackgroundHex(nav.colorToHex(col));
        }
    }

    // Multi-select model import for the Empty scene.
    FileDialog {
        id: importModelsDialog
        title: qsTr("Import 3D models")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            "3D models (*.obj *.stl *.ply *.fbx *.dae *.3ds *.off *.gltf *.glb *.step *.stp)",
            "All files (*)"
        ]
        onAccepted: {
            let urls = [];
            if (selectedFiles !== undefined) urls = selectedFiles;
            else if (fileUrls !== undefined) urls = fileUrls;
            const paths = [];
            for (let i = 0; i < urls.length; ++i)
                paths.push(nav.toLocalPath(urls[i]));
            if (paths.length > 0 && nav.session
                    && nav.session.importModels !== undefined)
                nav.session.importModels(paths);
        }
    }
}
