import QtQuick
import QtQuick.Layouts
import IISPH.Theme

Rectangle {
    id: status
    property var session
    property string leftText: ""

    readonly property var c: Theme.colors
    readonly property var st: session ? session.status : ({})

    implicitHeight: 26
    color: c.chrome

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: status.c.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 10

        Text {
            text: status.leftText
            color: status.leftText.indexOf("Saved to ") === 0 ? status.c.ok : status.c.fg3
            font.pixelSize: 10
            font.weight: status.leftText.indexOf("Saved to ") === 0 ? Font.DemiBold : Font.Normal
        }

        Item { Layout.fillWidth: true }

        Text {
            text: (st.fluidCount !== undefined ? st.fluidCount : 0) + " fluid · "
                  + (st.boundaryCount !== undefined ? st.boundaryCount : 0) + " boundary"
            color: status.c.fg3
            font.pixelSize: 10
        }
    }
}
