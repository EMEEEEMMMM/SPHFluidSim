import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

RowLayout {
    id: row
    property string label: ""
    property string suffix: ""
    property real value: 0
    property real from: -1e9
    property real to: 1e9
    property int decimals: 3
    property bool integer: false

    signal edited(real newValue)

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property int fieldWidth: 128

    implicitHeight: 32
    spacing: 8

    function formatNumber(v) {
        return integer ? Math.round(v).toString() : Number(v).toFixed(decimals);
    }

    function commitText() {
        const parsed = Number(input.text.replace(",", "."));
        if (Number.isFinite(parsed)) {
            value = Math.min(to, Math.max(from, integer ? Math.round(parsed) : parsed));
        }
        input.text = formatNumber(value);
        row.edited(value);
    }

    Text {
        Layout.preferredWidth: 150
        Layout.maximumWidth: 150
        text: row.label
        color: row.c.fg2
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    Rectangle {
        Layout.preferredWidth: row.fieldWidth
        Layout.preferredHeight: 28
        radius: row.m.square ? 0 : row.m.radiusSm
        color: row.c.field
        border.width: input.activeFocus ? 2 : 1
        border.color: input.activeFocus ? row.c.accent : row.c.border

        TextField {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: row.suffix !== "" ? suffixLabel.implicitWidth + 14 : 8
            padding: 0
            color: row.c.fg
            placeholderTextColor: row.c.fg3
            selectionColor: row.c.accent
            selectedTextColor: row.c.accentFg
            font.pixelSize: 12
            font.family: row.m.numberFamily
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            inputMethodHints: row.integer ? Qt.ImhDigitsOnly : Qt.ImhFormattedNumbersOnly
            validator: DoubleValidator {
                bottom: row.from
                top: row.to
                decimals: row.integer ? 0 : row.decimals
                notation: DoubleValidator.StandardNotation
            }
            Component.onCompleted: text = row.formatNumber(row.value)
            onTextEdited: {
                const parsed = Number(text.replace(",", "."));
                if (Number.isFinite(parsed)) {
                    row.value = Math.min(row.to, Math.max(row.from, row.integer ? Math.round(parsed) : parsed));
                    row.edited(row.value);
                }
            }
            onEditingFinished: row.commitText()
            background: Item {}
        }

        Text {
            id: suffixLabel
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            visible: row.suffix !== ""
            text: row.suffix
            color: row.c.fg3
            font.pixelSize: 11
        }
    }

    Item { Layout.fillWidth: true }

    onValueChanged: {
        if (!input.activeFocus)
            input.text = formatNumber(value);
    }
}
