import QtQuick
import IISPH.Theme

Item {
    id: toggle
    property bool checked: false

    signal toggled(bool checked)

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitWidth: 34
    implicitHeight: 20

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 2
        anchors.bottomMargin: 2
        radius: m.square ? 0 : height / 2
        color: toggle.checked ? toggle.c.switchOn : toggle.c.track
        border.width: toggle.checked ? 0 : 1
        border.color: toggle.c.border
    }

    Rectangle {
        id: knob
        width: 16
        height: 16
        y: 2
        x: toggle.checked ? parent.width - width - 2 : 2
        radius: toggle.m.square ? 0 : 8
        color: "#FFFFFF"
        border.width: 1
        border.color: toggle.m.square ? toggle.c.fg : Qt.rgba(0, 0, 0, 0.18)

        Behavior on x {
            NumberAnimation { duration: 140; easing.type: Easing.InOutQuad }
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            toggle.checked = !toggle.checked;
            toggle.toggled(toggle.checked);
        }
    }
}
