import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

Popup {
    id: dialog
    property string title: ""
    property string bodyText: ""
    property string linkText: ""
    property string linkUrl: ""

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 430
    modal: true
    focus: true
    padding: 18
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    background: Rectangle {
        radius: dialog.m.radiusLg
        color: dialog.c.card
        border.width: 1
        border.color: dialog.c.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            Layout.fillWidth: true
            text: dialog.title
            color: dialog.c.fg
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            text: dialog.bodyText
            textFormat: Text.RichText
            color: dialog.c.fg2
            font.pixelSize: 11
            lineHeight: 1.5
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: dialog.linkUrl !== ""
            text: dialog.linkText !== "" ? dialog.linkText : dialog.linkUrl
            color: dialog.c.accent
            font.pixelSize: 11
            font.underline: true
            wrapMode: Text.WrapAnywhere

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: Qt.openUrlExternally(dialog.linkUrl)
            }
        }

        Button {
            id: closeButton
            Layout.alignment: Qt.AlignRight
            implicitWidth: 84
            implicitHeight: 28
            text: qsTr("Close")
            Accessible.name: qsTr("Close dialog")
            onClicked: dialog.close()

            background: Rectangle {
                radius: dialog.m.square ? 0 : dialog.m.radiusSm
                color: closeButton.hovered ? dialog.c.chip : dialog.c.field
                border.width: 1
                border.color: dialog.c.borderStrong
            }
            contentItem: Text {
                text: closeButton.text
                color: dialog.c.fg
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
