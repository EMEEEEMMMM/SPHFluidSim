import QtQuick
import IISPH.View
import IISPH.Theme

Item {
    id: btn
    property int kind: 0
    property bool circular: false
    property bool primary: false
    property bool active: false
    property real iconRatio: 0.55

    signal clicked()

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    property bool hovered: false

    implicitWidth: 28
    implicitHeight: 28

    Rectangle {
        anchors.fill: parent
        radius: btn.circular ? width / 2 : btn.m.radiusSm
        color: btn.primary
               ? (btn.hovered ? Qt.lighter(btn.c.accent, 1.12) : btn.c.accent)
               : (btn.active ? btn.c.accentSoft : (btn.hovered ? btn.c.chip : "transparent"))
    }

    IconItem {
        anchors.centerIn: parent
        width: Math.round(Math.min(btn.width, btn.height) * btn.iconRatio)
        height: width
        kind: btn.kind
        color: btn.primary
               ? (btn.c.accentFg)
               : (btn.active ? btn.c.accent : (btn.hovered ? btn.c.fg : btn.c.fg2))
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onEntered: btn.hovered = true
        onExited: btn.hovered = false
        onClicked: btn.clicked()
    }
}
