import QtQuick
import QtQuick.Layouts
import IISPH.Theme

Rectangle {
    id: grid
    property var keys: []
    property var values: []
    // Optional per-cell value colour (e.g. density error banding).
    property var colors: []

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    radius: m.square ? 0 : m.radiusSm
    color: "transparent"
    border.width: 1
    border.color: c.border
    implicitHeight: layout.implicitHeight

    GridLayout {
        id: layout
        anchors.fill: parent
        columns: 2
        rowSpacing: 0
        columnSpacing: 0

        Repeater {
            model: grid.keys

            Item {
                id: cell
                required property int index
                required property string modelData

                Layout.fillWidth: true
                Layout.preferredHeight: 54

                Rectangle {
                    visible: cell.index % 2 === 0
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: grid.c.border
                }
                Rectangle {
                    visible: cell.index < 2
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: grid.c.border
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    anchors.topMargin: 7
                    anchors.bottomMargin: 7
                    spacing: 1

                    Text {
                        text: grid.m.titleCaps ? cell.modelData.toUpperCase() : cell.modelData
                        color: grid.c.fg3
                        font.pixelSize: 9
                        font.letterSpacing: 0.6
                    }
                    Item { Layout.fillHeight: true }
                    Text {
                        text: grid.values[cell.index] !== undefined ? grid.values[cell.index] : "—"
                        color: grid.colors[cell.index] !== undefined ? grid.colors[cell.index] : grid.c.fg
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        font.family: grid.m.numberFamily
                    }
                }
            }
        }
    }
}
