import QtQuick
import QtQuick.Layouts
import IISPH.Theme

Item {
    id: card
    property string title: ""
    property string badge: ""
    property bool badgeLive: false
    default property alias content: body.data

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitWidth: 220
    implicitHeight: layout.implicitHeight + m.cardPad * 2

    Rectangle {
        visible: card.m.shadow && card.c.shadow.a > 0
        x: 0
        y: 2
        width: parent.width
        height: parent.height
        radius: card.m.radiusLg
        color: card.c.shadow
    }

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: card.m.radiusLg
        color: card.c.card
        border.width: 1
        border.color: card.c.border
    }

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: card.m.cardPad
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Text {
                text: card.m.titleCaps ? card.title.toUpperCase() : card.title
                color: card.c.fg3
                font.pixelSize: 10
                font.weight: Font.DemiBold
                font.letterSpacing: card.m.square ? 1.0 : 0.7
            }
            Item { Layout.fillWidth: true }

            Rectangle {
                visible: card.badge !== ""
                implicitWidth: Math.max(34, badgeLabel.implicitWidth + 14)
                implicitHeight: 17
                radius: card.m.square ? 0 : height / 2
                color: card.badgeLive ? card.c.accentSoft : card.c.chip
                border.width: 1
                border.color: card.badgeLive ? card.c.switchOn : card.c.border

                Text {
                    id: badgeLabel
                    anchors.centerIn: parent
                    text: card.badge
                    color: card.badgeLive ? card.c.switchOn : card.c.fg2
                    font.pixelSize: 9
                    font.family: card.m.numberFamily
                }
            }
        }

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 7
        }
    }
}
