import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

// Shared camera editor. Used in full by RenderDialog (render.camera config) and
// in "presets-only" mode by the live viewport Camera card. The session is the
// single source of truth for the render config; cameraSource (ViewportItem) is
// only read explicitly by "Use current view".
ColumnLayout {
    id: root
    property var session
    property var cameraSource        // Main.qml viewportProbe
    property bool live: false        // true => presets drive the live viewport
    property bool showFields: true   // numeric yaw/pitch/distance/target/fov
    property bool showActions: true  // "Use current view" / "Reset"
    property string activePreset: ""

    signal configChanged()

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property var presetNames:
        ["Front", "Back", "Left", "Right", "Top", "Iso", "Current"]

    spacing: 8

    // Camera config shape expected by session.setCameraConfig() / render.camera.
    function config() {
        return {
            yaw: yawField.value,
            pitch: pitchField.value,
            distance: distanceField.value,
            targetX: targetXField.value,
            targetY: targetYField.value,
            targetZ: targetZField.value,
            fov: fovField.value
        };
    }

    function applyConfig(cfg) {
        if (!cfg) return;
        if (cfg.yaw !== undefined) yawField.value = Number(cfg.yaw);
        if (cfg.pitch !== undefined) pitchField.value = Number(cfg.pitch);
        if (cfg.distance !== undefined) distanceField.value = Number(cfg.distance);
        if (cfg.targetX !== undefined) targetXField.value = Number(cfg.targetX);
        if (cfg.targetY !== undefined) targetYField.value = Number(cfg.targetY);
        if (cfg.targetZ !== undefined) targetZField.value = Number(cfg.targetZ);
        if (cfg.fov !== undefined) fovField.value = Number(cfg.fov);
    }

    // The session's cameraState is the render config and the only implicit
    // source. Never copy the live viewport here: "Use current view" is the
    // explicit path for that.
    function refresh() {
        if (root.session && root.session.cameraState !== undefined) {
            const cfg = root.session.cameraState;
            if (cfg) root.applyConfig(cfg);
        }
    }

    function pushConfig() {
        if (root.session && root.session.setCameraConfig !== undefined) {
            root.session.setCameraConfig(root.config());
            root.configChanged();
        }
    }

    function applyPreset(name) {
        root.activePreset = name;
        if (root.live && root.cameraSource
                && root.cameraSource.applyCameraPreset !== undefined) {
            root.cameraSource.applyCameraPreset(name);
        } else if (root.session && root.session.applyCameraPreset !== undefined) {
            root.session.applyCameraPreset(name);
        }
        root.refresh();
        root.configChanged();
    }

    // Live mode: reset the actual viewport camera (fixed Empty eye / scene
    // default), not just the orientation. Render mode keeps the Iso preset.
    function resetView() {
        if (root.live && root.cameraSource
                && root.cameraSource.resetCamera !== undefined) {
            root.cameraSource.resetCamera();
            root.activePreset = "";
            root.configChanged();
            return;
        }
        root.applyPreset("Iso");
    }

    // "Use current view": read the live viewport camera, copy it into the
    // render config and into the local fields (viewportProbe.currentCamera()
    // -> session.setCameraConfig()).
    function useCurrentView() {
        let cfg = null;
        if (root.cameraSource && root.cameraSource.currentCamera !== undefined)
            cfg = root.cameraSource.currentCamera();
        if (cfg) {
            root.applyConfig(cfg);
            root.pushConfig();
        } else if (root.session && root.session.useCurrentViewCamera !== undefined) {
            root.session.useCurrentViewCamera();
            root.refresh();
            root.configChanged();
        }
        root.activePreset = "Current";
    }

    Connections {
        target: root.session
        // The dialog can be constructed before the context session is set.
        ignoreUnknownSignals: true
        function onCameraStateChanged() { root.refresh(); }
    }

    Component.onCompleted: root.refresh()

    GridLayout {
        Layout.fillWidth: true
        columns: 4
        columnSpacing: 5
        rowSpacing: 5

        Repeater {
            model: root.presetNames

            Rectangle {
                required property int index
                required property string modelData

                Layout.fillWidth: true
                Layout.preferredHeight: 24
                radius: root.m.square ? 0 : root.m.radiusSm
                color: root.activePreset === modelData
                       ? root.c.segActive
                       : (presetHover.containsMouse ? root.c.chip : root.c.field)
                border.width: 1
                border.color: root.activePreset === modelData
                              ? root.c.accent : root.c.border

                Text {
                    anchors.centerIn: parent
                    text: modelData
                    color: root.activePreset === modelData
                           ? root.c.segActiveFg : root.c.fg2
                    font.pixelSize: 10
                    font.weight: root.activePreset === modelData
                                 ? Font.DemiBold : Font.Medium
                }

                MouseArea {
                    id: presetHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.applyPreset(modelData)
                }
            }
        }
    }

    GridLayout {
        Layout.fillWidth: true
        visible: root.showFields
        columns: 2
        columnSpacing: 14
        rowSpacing: 4

        NumberField {
            id: yawField
            Layout.fillWidth: true
            label: qsTr("Yaw")
            suffix: "°"
            value: 45
            from: -360; to: 360; decimals: 1
            onEdited: root.pushConfig()
        }
        NumberField {
            id: pitchField
            Layout.fillWidth: true
            label: qsTr("Pitch")
            suffix: "°"
            value: 25
            from: -89; to: 89; decimals: 1
            onEdited: root.pushConfig()
        }
        NumberField {
            id: distanceField
            Layout.fillWidth: true
            label: qsTr("Distance")
            value: 4.2
            from: 0.01; to: 1000; decimals: 3
            onEdited: root.pushConfig()
        }
        NumberField {
            id: fovField
            Layout.fillWidth: true
            label: qsTr("FOV")
            suffix: "°"
            value: 42
            from: 10; to: 120; decimals: 1
            onEdited: root.pushConfig()
        }
        NumberField {
            id: targetXField
            Layout.fillWidth: true
            label: qsTr("Target X")
            value: 1
            from: -100000; to: 100000; decimals: 3
            onEdited: root.pushConfig()
        }
        NumberField {
            id: targetYField
            Layout.fillWidth: true
            label: qsTr("Target Y")
            value: 0.55
            from: -100000; to: 100000; decimals: 3
            onEdited: root.pushConfig()
        }
        NumberField {
            id: targetZField
            Layout.fillWidth: true
            label: qsTr("Target Z")
            value: 0.5
            from: -100000; to: 100000; decimals: 3
            onEdited: root.pushConfig()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.showActions
        spacing: 8

        DialogButton {
            Layout.fillWidth: true
            text: qsTr("Use current view")
            onClicked: root.useCurrentView()
        }
        DialogButton {
            Layout.fillWidth: true
            text: qsTr("Reset")
            onClicked: root.resetView()
        }
    }
}
