import QtQuick
import IISPH.Theme

Item {
    id: swatch
    property int themeIndex: 0
    property bool compact: false

    readonly property var p: Theme.preview(themeIndex)

    implicitWidth: compact ? 18 : 34
    implicitHeight: compact ? 18 : 34

    Rectangle {
        anchors.fill: parent
        radius: swatch.p.square ? 0 : (swatch.compact ? 4 : 6)
        color: swatch.p.bg
        border.width: 1
        border.color: swatch.p.border
    }

    Rectangle {
        x: swatch.compact ? 4 : 7
        y: swatch.compact ? 4 : 7
        width: swatch.compact ? 10 : 20
        height: swatch.compact ? 7 : 13
        radius: swatch.p.square ? 0 : 3
        color: swatch.p.card
        border.width: 1
        border.color: swatch.p.border
    }

    Rectangle {
        x: swatch.compact ? 4 : 7
        y: swatch.compact ? 13 : 24
        width: swatch.compact ? 7 : 12
        height: swatch.compact ? 2 : 3
        radius: swatch.p.square ? 0 : 2
        color: swatch.p.accent
    }
}
