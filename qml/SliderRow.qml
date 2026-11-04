import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import IISPH.Theme

ColumnLayout {
    id: row
    property string label: ""
    property string suffix: ""
    property real from: 0
    property real to: 1
    property real value: 0
    property int decimals: 3

    signal edited(real newValue)

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property int trackHeight: Math.max(2, Math.round(m.trackH))
    readonly property int handleWidth: m.square ? 8 : 14
    readonly property int handleHeight: m.square ? 16 : 14

    spacing: 2

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Text {
            text: row.label
            color: row.c.fg2
            font.pixelSize: 11
        }
        Item { Layout.fillWidth: true }
        Text {
            text: row.value.toFixed(row.decimals) + row.suffix
            color: row.c.fg
            font.pixelSize: 11
            font.weight: Font.Medium
            font.family: row.m.numberFamily
        }
    }

    Slider {
        id: slider
        Layout.fillWidth: true
        Layout.preferredHeight: 18
        from: row.from
        to: row.to
        stepSize: 0
        onMoved: row.edited(value)

        Binding on value {
            when: !slider.pressed
            value: row.value
        }

        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: slider.availableWidth
            height: row.trackHeight
            radius: row.m.square ? 0 : height / 2
            color: row.c.track

            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: parent.radius
                color: row.c.accent
            }
        }

        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            implicitWidth: row.handleWidth
            implicitHeight: row.handleHeight
            radius: row.m.square ? 0 : (row.m.backdrop ? 7 : 3)
            color: row.c.sliderHandle
            border.width: 1
            border.color: row.c.sliderHandleBorder
        }
    }
}
