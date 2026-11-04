import QtQuick
import IISPH.View
import IISPH.Theme

Item {
    id: button
    property int kind: 10
    property bool danger: false
    property bool hovered: false

    signal clicked()

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitWidth: 34
    implicitHeight: 28
    Accessible.role: Accessible.Button
    Accessible.name: kind === 10 ? qsTr("Minimize")
                     : kind === 11 ? qsTr("Maximize")
                     : qsTr("Close")

    Rectangle {
        anchors.fill: parent
        anchors.margins: 2
        radius: button.m.square ? 0 : button.m.radiusSm
        color: button.hovered
               ? (button.danger ? "#D93B32" : button.c.chip)
               : "transparent"

        Behavior on color {
            ColorAnimation { duration: 100; easing.type: Easing.OutCubic }
        }
    }

    IconItem {
        anchors.centerIn: parent
        width: 13
        height: 13
        kind: button.kind
        color: button.hovered && button.danger ? "#FFFFFF"
                                               : (button.hovered ? button.c.fg : button.c.fg2)
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onEntered: button.hovered = true
        onExited: button.hovered = false
        onClicked: button.clicked()
    }
}
