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
               : (action.hovered ? action.c.chip : "transparent")
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
                color: action.danger ? action.c.danger : action.c.fg
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
            visible: action.selected
            width: 7
            height: 7
            radius: action.m.square ? 0 : 3.5
            color: action.c.accent
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
