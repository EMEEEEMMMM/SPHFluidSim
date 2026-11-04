import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

Button {
    id: root
    property int menuWidth: 240
    default property alias content: column.data

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitWidth: label.implicitWidth + 24
    implicitHeight: 24
    padding: 0
    hoverEnabled: true
    Accessible.name: text

    function openMenu() { menu.open(); }
    function closeMenu() { menu.close(); }

    onClicked: menu.opened ? menu.close() : menu.open()

    background: Rectangle {
        radius: root.m.square ? 0 : root.m.radiusSm
        color: root.hovered || menu.visible ? root.c.chip : "transparent"

        Behavior on color {
            ColorAnimation { duration: 110; easing.type: Easing.OutCubic }
        }
    }

    contentItem: Text {
        id: label
        text: root.text
        color: root.hovered || menu.visible ? root.c.fg : root.c.fg2
        font.pixelSize: 11
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    Popup {
        id: menu
        x: 0
        y: root.height + 5
        width: root.menuWidth
        implicitHeight: column.implicitHeight + 12
        padding: 6
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: root.m.radiusLg
            color: root.c.card
            border.width: 1
            border.color: root.c.borderStrong
        }

        contentItem: ColumnLayout {
            id: column
            spacing: 2
        }
    }
}
