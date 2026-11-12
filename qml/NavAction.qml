import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

Button {
    id: action
    property string shortcut: ""
    property string subtitle: ""
    property int themeIndex: -1
    property bool selected: false
    property bool danger: false
    // Menu items that express a mutually exclusive choice (scene list) show a
    // check mark instead of the theme-selection dot.
    property bool showCheck: false

    signal selectedAction()

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    Layout.fillWidth: true
    implicitHeight: subtitle !== "" ? 48 : 32
    padding: 6
    hoverEnabled: true
    Accessible.name: text

    background: Rectangle {
        radius: action.m.square ? 0 : action.m.radiusSm
        color: action.selected
               ? action.c.accentSoft
               : (action.enabled && action.hovered ? action.c.chip : "transparent")
        border.width: action.selected ? 1 : 0
        border.color: action.c.accent
    }

    contentItem: RowLayout {
        spacing: 8

        ThemeSwatch {
            visible: action.themeIndex >= 0
            themeIndex: Math.max(0, action.themeIndex)
            Layout.alignment: Qt.AlignVCenter
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1

            Text {
                Layout.fillWidth: true
                text: action.text
                color: !action.enabled ? action.c.fg3
                       : (action.danger ? action.c.danger : action.c.fg)
                font.pixelSize: 11
                font.weight: action.selected ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
            Text {
                visible: action.subtitle !== ""
                Layout.fillWidth: true
                text: action.subtitle
                color: action.c.fg3
                font.pixelSize: 9
                elide: Text.ElideRight
            }
        }

        Rectangle {
            visible: action.selected && !action.showCheck
            Layout.preferredWidth: 7
            Layout.preferredHeight: 7
            radius: action.m.square ? 0 : 3.5
            color: action.c.accent
        }

        Text {
            visible: action.showCheck && action.selected
            text: "✓"
            color: action.c.accent
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }

        Text {
            visible: action.shortcut !== ""
            text: action.shortcut
            color: action.c.fg3
            font.pixelSize: 9
            font.family: action.m.numberFamily
        }
    }

    onClicked: action.selectedAction()
}
