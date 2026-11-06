import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

GroupBox {
    id: section
    property string heading: ""
    default property alias content: body.data

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    Layout.fillWidth: true
    padding: 12
    topPadding: 34

    label: Text {
        x: 12
        y: 10
        text: section.heading
        color: section.c.fg3
        font.pixelSize: 10
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.7
    }

    background: Rectangle {
        radius: section.m.radiusLg
        color: section.c.card
        border.width: 1
        border.color: section.c.border
    }

    contentItem: ColumnLayout {
        id: body
        spacing: 8
    }
}
