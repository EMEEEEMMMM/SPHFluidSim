import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

// Compact numeric input for the side-panel cards. Same editing contract as
// RenderField.qml (label + value + range + edited(real)), just narrower so it
// fits into a 272-300 px column. A programmatic value change updates the text
// while the field is not focused, so external refreshes do not fight typing.
RowLayout {
    id: row
    property string label: ""
    property string suffix: ""
    property real value: 0
    property real from: -1e9
    property real to: 1e9
    property int decimals: 3
    property bool integer: false
    property int labelWidth: 88
    property int fieldWidth: 96
    // Side-panel rows stretch with a trailing spacer; compact XYZ groups
    // (three fields per row) switch this off so they sit flush together.
    property bool trailingFill: true

    signal edited(real newValue)

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    implicitHeight: 26
    spacing: 6

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
        Layout.preferredWidth: row.labelWidth
        Layout.maximumWidth: row.labelWidth
        text: row.label
        color: row.c.fg2
        font.pixelSize: 11
        elide: Text.ElideRight
    }
    Rectangle {
        Layout.preferredWidth: row.fieldWidth
        Layout.preferredHeight: 24
        radius: row.m.square ? 0 : row.m.radiusSm
        color: row.c.field
        border.width: input.activeFocus ? 2 : 1
        border.color: input.activeFocus ? row.c.accent : row.c.border

        TextField {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: row.suffix !== "" ? suffixLabel.implicitWidth + 10 : 6
            padding: 0
            color: row.c.fg
            selectionColor: row.c.accent
            selectedTextColor: row.c.accentFg
            font.pixelSize: 11
            font.family: row.m.numberFamily
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            inputMethodHints: row.integer ? Qt.ImhDigitsOnly : Qt.ImhFormattedNumbersOnly
            Accessible.name: row.label !== "" ? row.label : qsTr("Numeric value")
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
                    row.value = Math.min(row.to, Math.max(row.from,
                                        row.integer ? Math.round(parsed) : parsed));
                    row.edited(row.value);
                }
            }
            onEditingFinished: row.commitText()
            background: Item {}
        }

        Text {
            id: suffixLabel
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: row.suffix !== ""
            text: row.suffix
            color: row.c.fg3
            font.pixelSize: 10
        }
    }

    Item { Layout.fillWidth: true; visible: row.trailingFill }

    onValueChanged: {
        if (!input.activeFocus)
            input.text = formatNumber(value);
    }
}
