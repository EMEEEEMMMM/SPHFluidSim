import QtQuick
import QtQuick.Layouts
import IISPH.Theme

RowLayout {
    id: row
    property string label: ""
    property var options: []
    property int currentIndex: 0

    signal activated(int index)

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property int radiusValue: m.square ? 0 : Math.max(4, m.radiusSm)

    implicitHeight: 30
    spacing: 8

    Text {
        Layout.fillWidth: true
        Layout.minimumWidth: 110
        text: row.label
        color: row.c.fg2
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    Rectangle {
        Layout.preferredWidth: 196
        Layout.preferredHeight: 28
        radius: row.radiusValue
        color: row.c.field
        border.width: 1
        border.color: row.c.border

        Row {
            id: buttons
            anchors.fill: parent
            anchors.margins: 3

            Repeater {
                model: row.options

                Item {
                    required property int index
                    required property string modelData

                    width: buttons.width / Math.max(1, row.options.length)
                    height: buttons.height

                    Rectangle {
                        anchors.fill: parent
                        radius: row.radiusValue
                        color: index === row.currentIndex
                               ? row.c.segActive
                               : (hover.containsMouse ? row.c.chip : "transparent")
                    }

                    Text {
                        anchors.centerIn: parent
                        text: parent.modelData
                        color: parent.index === row.currentIndex
                               ? row.c.segActiveFg
                               : (hover.containsMouse ? row.c.fg : row.c.fg2)
                        font.pixelSize: 11
                        font.weight: parent.index === row.currentIndex ? Font.DemiBold : Font.Medium
                    }

                    MouseArea {
                        id: hover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            row.currentIndex = parent.index;
                            row.activated(row.currentIndex);
                        }
                    }
                }
            }
        }
    }
}
