import QtQuick
import QtQuick.Controls.Basic
import IISPH.Theme

Button {
    id: button
    property bool primary: false
    property bool danger: false

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitWidth: Math.max(100, buttonLabel.implicitWidth + 24)
    implicitHeight: 32
    padding: 8
    hoverEnabled: true
    Accessible.name: text

    background: Rectangle {
        radius: button.m.square ? 0 : button.m.radiusSm
        color: !button.enabled
               ? button.c.field
               : (button.primary ? button.c.accent
                                 : (button.danger && button.hovered ? button.c.danger
                                                                    : (button.hovered ? button.c.chip : button.c.field)))
        border.width: button.visualFocus ? 2 : 1
        border.color: !button.enabled
                      ? button.c.border
                      : (button.primary ? button.c.accent : (button.danger ? button.c.danger : button.c.borderStrong))
    }

    contentItem: Text {
        id: buttonLabel
        text: button.text
        color: !button.enabled
               ? button.c.fg3
               : (button.primary ? button.c.accentFg
                                 : (button.danger
                                    ? (button.hovered ? "#FFFFFF" : button.c.danger)
                                    : button.c.fg))
        font.pixelSize: 13
        font.weight: button.primary ? Font.DemiBold : Font.Medium
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
