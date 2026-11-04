import QtQuick
import QtQuick.Layouts
import IISPH.Theme

Rectangle {
    id: pill
    property string text: ""
    property bool showDot: true

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitWidth: content.implicitWidth + 18
    implicitHeight: 24
    radius: m.square ? 0 : height / 2
    color: c.hud
    border.width: 1
    border.color: c.hudBorder

    RowLayout {
        id: content
        anchors.centerIn: parent
        spacing: 6

        Rectangle {
            visible: pill.showDot
            width: 6
            height: 6
            radius: 3
            color: pill.c.ok
        }

        Text {
            text: pill.text
            color: "#E8ECF4"
            font.pixelSize: 11
        }
    }
}
