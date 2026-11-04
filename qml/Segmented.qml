import QtQuick
import IISPH.Theme

Item {
    id: seg
    property var options: []
    property int currentIndex: 0
    property int fontSize: 11
    property bool compact: false

    signal activated(int index)

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property real radiusValue: m.square ? 0 : Math.max(4, m.radiusSm)

    implicitHeight: compact ? Math.max(24, m.controlH - 4) : m.controlH
    // NOTE: must not depend on row.implicitWidth — the row's cells size
    // themselves from row.width, so deriving the width from the row closes a
    // binding loop (measured: 100% CPU on one core, the window never appears).
    implicitWidth: Math.max(80, options.length * 72 + 6)
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        radius: seg.radiusValue
        color: seg.c.field
        border.width: 1
        border.color: seg.c.border
    }

    Row {
        id: row
        anchors.fill: parent
        anchors.margins: 3

        Repeater {
            model: seg.options

            Item {
                required property int index
                required property string modelData

                width: row.width / Math.max(1, seg.options.length)
                height: row.height

                Rectangle {
                    anchors.fill: parent
                    radius: seg.radiusValue
                    color: index === seg.currentIndex
                           ? seg.c.segActive
                           : (hoverArea.containsMouse ? seg.c.chip : "transparent")

                    Behavior on color {
                        ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    text: parent.modelData
                    font.pixelSize: seg.fontSize
                    font.weight: parent.index === seg.currentIndex ? Font.DemiBold : Font.Medium
                    color: parent.index === seg.currentIndex
                           ? seg.c.segActiveFg
                           : (hoverArea.containsMouse ? seg.c.fg : seg.c.fg2)
                }

                MouseArea {
                    id: hoverArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (seg.currentIndex === parent.index) return;
                        seg.currentIndex = parent.index;
                        seg.activated(parent.index);
                    }
                }
            }
        }
    }
}
