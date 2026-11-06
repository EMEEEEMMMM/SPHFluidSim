import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

CheckBox {
    id: control
    property string label: ""

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    Layout.preferredHeight: 26
    implicitHeight: 26
    text: label
    spacing: 8
    padding: 0
    hoverEnabled: true
    Accessible.name: label

    indicator: Rectangle {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 16
        height: 16
        radius: control.m.square ? 0 : 4
        color: control.checked ? control.c.accent : control.c.field
        border.width: control.visualFocus ? 2 : 1
        border.color: control.checked ? control.c.accent
                                      : (control.hovered ? control.c.borderStrong : control.c.border)

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: control.c.accentFg
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        color: control.enabled ? control.c.fg : control.c.fg3
        font.pixelSize: 12
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
