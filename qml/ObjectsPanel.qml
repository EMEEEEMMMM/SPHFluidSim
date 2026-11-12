import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import IISPH.Theme

// Empty-scene object manager. A Card holding the model list, a multi-select
// import dialog and the selected model's transform. Single selection, no undo.
Item {
    id: root
    property var session
    property bool importEnabled: true

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitWidth: 240
    implicitHeight: card.implicitHeight

    readonly property var objectList:
        root.session && root.session.objects !== undefined ? root.session.objects : []
    readonly property int selectedId:
        root.session && root.session.selectedModelId !== undefined
            ? root.session.selectedModelId : -1
    readonly property var selectedObject: {
        for (let i = 0; i < root.objectList.length; ++i)
            if (root.objectList[i].id === root.selectedId)
                return root.objectList[i];
        return null;
    }

    // Qt6 QtQuick.Dialogs returns QUrls; the C++ importModels() wants local
    // paths. fileUrls / selectMultiple are the Qt5/Qt.labs names kept as a
    // fallback so this works across both dialog implementations.
    function toLocalPath(u) {
        let s = String(u);
        if (s.indexOf("file:///") === 0) s = s.substring(8);
        else if (s.indexOf("file:/") === 0) s = s.substring(6);
        try { s = decodeURIComponent(s); } catch (e) {}
        if (s.length > 1 && s[1] !== ":" && s[0] !== "/")
            s = "/" + s;
        return s;
    }

    function refreshTransform() {
        const o = root.selectedObject;
        if (!o) return;
        const pos = o.position !== undefined ? o.position : [0, 0, 0];
        const rot = o.rotationEuler !== undefined ? o.rotationEuler : [0, 0, 0];
        posXField.value = Number(pos[0] !== undefined ? pos[0] : 0);
        posYField.value = Number(pos[1] !== undefined ? pos[1] : 0);
        posZField.value = Number(pos[2] !== undefined ? pos[2] : 0);
        rotXField.value = Number(rot[0] !== undefined ? rot[0] : 0);
        rotYField.value = Number(rot[1] !== undefined ? rot[1] : 0);
        rotZField.value = Number(rot[2] !== undefined ? rot[2] : 0);
        scaleField.value = Number(o.scale !== undefined ? o.scale : 1);
    }

    function applyPosition() {
        if (!root.session || root.selectedId < 0
                || root.session.setModelPosition === undefined) return;
        root.session.setModelPosition(root.selectedId,
                                      posXField.value, posYField.value, posZField.value);
    }
    function applyRotation() {
        if (!root.session || root.selectedId < 0
                || root.session.setModelRotationEuler === undefined) return;
        root.session.setModelRotationEuler(root.selectedId,
                                           rotXField.value, rotYField.value, rotZField.value);
    }
    function applyScale() {
        if (!root.session || root.selectedId < 0
                || root.session.setModelScale === undefined) return;
        root.session.setModelScale(root.selectedId, scaleField.value);
    }
    function resetTransform() {
        if (root.session && root.selectedId >= 0
                && root.session.resetModelTransform !== undefined)
            root.session.resetModelTransform(root.selectedId);
    }
    function deleteSelected() {
        if (root.session && root.selectedId >= 0
                && root.session.removeModel !== undefined)
            root.session.removeModel(root.selectedId);
    }
    function clearAll() {
        if (root.session && root.session.clearModels !== undefined)
            root.session.clearModels();
    }

    Connections {
        target: root.session
        function onObjectsChanged() { root.refreshTransform(); }
        function onSelectionChanged() { root.refreshTransform(); }
    }

    Component.onCompleted: root.refreshTransform()

    Card {
        id: card
        anchors.fill: parent
        title: "Objects"
        badge: root.objectList.length > 0 ? String(root.objectList.length) : ""

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            DialogButton {
                Layout.fillWidth: true
                text: qsTr("Import")
                enabled: root.importEnabled
                onClicked: importDialog.open()
            }
            DialogButton {
                Layout.fillWidth: true
                text: qsTr("Clear")
                enabled: root.importEnabled && root.objectList.length > 0
                onClicked: root.clearAll()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3

            Repeater {
                model: root.objectList

                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 6

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        radius: root.m.square ? 0 : root.m.radiusSm
                        color: root.selectedId === modelData.id
                               ? root.c.accentSoft
                               : (nameHover.containsMouse ? root.c.chip : "transparent")
                        border.width: root.selectedId === modelData.id ? 1 : 0
                        border.color: root.c.accent
                        Accessible.name: modelData.name !== undefined
                                         ? modelData.name : ("Model " + modelData.id)

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name !== undefined
                                  ? modelData.name : ("Model " + modelData.id)
                            color: modelData.visible ? root.c.fg : root.c.fg3
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }

                        MouseArea {
                            id: nameHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.session && root.session.selectModel !== undefined)
                                    root.session.selectModel(modelData.id);
                            }
                        }
                    }

                    // Visibility eye: open eye = visible, crossed = hidden.
                    Item {
                        implicitWidth: 20
                        implicitHeight: 20
                        Accessible.name: qsTr("Toggle model visibility")

                        Rectangle {
                            anchors.centerIn: parent
                            width: 14
                            height: 9
                            radius: 4.5
                            color: "transparent"
                            border.width: 1
                            border.color: modelData.visible ? root.c.accent : root.c.fg3
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 5
                            height: 5
                            radius: 2.5
                            color: modelData.visible ? root.c.accent : "transparent"
                            border.width: modelData.visible ? 0 : 1
                            border.color: root.c.fg3
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            visible: !modelData.visible
                            width: 16
                            height: 1
                            color: root.c.fg3
                            rotation: 45
                        }
                        MouseArea {
                            id: eyeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.session && root.session.setModelVisible !== undefined)
                                    root.session.setModelVisible(modelData.id,
                                                                 !modelData.visible);
                            }
                        }
                    }

                    // Remove: small hover target rather than a bare glyph.
                    Rectangle {
                        implicitWidth: 20
                        implicitHeight: 20
                        radius: root.m.square ? 0 : 4
                        color: deleteArea.containsMouse ? root.c.accentSoft : "transparent"
                        Accessible.name: qsTr("Remove model")

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: root.c.danger
                            font.pixelSize: 11
                        }
                        MouseArea {
                            id: deleteArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.session && root.session.removeModel !== undefined)
                                    root.session.removeModel(modelData.id);
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.objectList.length === 0
                text: root.importEnabled
                      ? qsTr("No models — import one to get started.")
                      : qsTr("Import is available in the Empty scene.")
                color: root.c.fg3
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            visible: root.selectedObject !== null
            color: root.c.border
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: root.selectedObject !== null
            spacing: 4

            Text {
                text: qsTr("Position")
                color: root.c.fg3
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                NumberField {
                    id: posXField
                    Layout.preferredWidth: implicitWidth
                    label: "X"
                    labelWidth: 12
                    fieldWidth: 52
                    trailingFill: false
                    from: -100000; to: 100000; decimals: 3
                    onEdited: root.applyPosition()
                }
                NumberField {
                    id: posYField
                    Layout.preferredWidth: implicitWidth
                    label: "Y"
                    labelWidth: 12
                    fieldWidth: 52
                    trailingFill: false
                    from: -100000; to: 100000; decimals: 3
                    onEdited: root.applyPosition()
                }
                NumberField {
                    id: posZField
                    Layout.preferredWidth: implicitWidth
                    label: "Z"
                    labelWidth: 12
                    fieldWidth: 52
                    trailingFill: false
                    from: -100000; to: 100000; decimals: 3
                    onEdited: root.applyPosition()
                }
                Item { Layout.fillWidth: true }
            }

            Text {
                text: qsTr("Rotation (deg)")
                color: root.c.fg3
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                Layout.topMargin: 4
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                NumberField {
                    id: rotXField
                    Layout.preferredWidth: implicitWidth
                    label: "X"
                    labelWidth: 12
                    fieldWidth: 52
                    trailingFill: false
                    from: -360; to: 360; decimals: 1
                    onEdited: root.applyRotation()
                }
                NumberField {
                    id: rotYField
                    Layout.preferredWidth: implicitWidth
                    label: "Y"
                    labelWidth: 12
                    fieldWidth: 52
                    trailingFill: false
                    from: -360; to: 360; decimals: 1
                    onEdited: root.applyRotation()
                }
                NumberField {
                    id: rotZField
                    Layout.preferredWidth: implicitWidth
                    label: "Z"
                    labelWidth: 12
                    fieldWidth: 52
                    trailingFill: false
                    from: -360; to: 360; decimals: 1
                    onEdited: root.applyRotation()
                }
                Item { Layout.fillWidth: true }
            }

            NumberField {
                id: scaleField
                Layout.fillWidth: true
                label: qsTr("Scale")
                labelWidth: 60
                value: 1
                from: 0.001; to: 1000; decimals: 3
                onEdited: root.applyScale()
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                DialogButton {
                    Layout.fillWidth: true
                    text: qsTr("Reset")
                    onClicked: root.resetTransform()
                }
                DialogButton {
                    Layout.fillWidth: true
                    text: qsTr("Delete")
                    danger: true
                    onClicked: root.deleteSelected()
                }
            }
        }
    }

    FileDialog {
        id: importDialog
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
                paths.push(root.toLocalPath(urls[i]));
            if (paths.length > 0 && root.session
                    && root.session.importModels !== undefined)
                root.session.importModels(paths);
        }
    }
}
