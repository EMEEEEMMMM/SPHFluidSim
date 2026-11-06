import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import IISPH.Theme

Popup {
    id: dialog
    property var job

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 560
    height: 250
    modal: true
    focus: true
    padding: 18
    closePolicy: Popup.NoAutoClose

    function formatTime(seconds) {
        if (!Number.isFinite(seconds) || seconds < 0) return "--:--";
        const total = Math.round(seconds);
        const h = Math.floor(total / 3600);
        const mm = Math.floor((total % 3600) / 60);
        const ss = total % 60;
        const pad = (n) => (n < 10 ? "0" + n : "" + n);
        return h > 0 ? h + ":" + pad(mm) + ":" + pad(ss) : mm + ":" + pad(ss);
    }

    background: Rectangle {
        radius: dialog.m.radiusLg
        color: dialog.c.card
        border.width: 1
        border.color: dialog.c.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Text {
                    text: qsTr("Rendering")
                    color: dialog.c.fg
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }
                Text {
                    text: dialog.job ? dialog.job.statusText : ""
                    color: dialog.job && dialog.job.paused ? dialog.c.warn : dialog.c.fg2
                    font.pixelSize: 13
                }
            }

            Text {
                text: dialog.job ? (dialog.job.frame + " / " + dialog.job.totalFrames + " " + qsTr("frames")) : ""
                color: dialog.c.fg2
                font.pixelSize: 13
                font.family: dialog.m.numberFamily
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: qsTr("Overall")
                    color: dialog.c.fg2
                    font.pixelSize: 13
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: dialog.job ? Math.round(dialog.job.overallProgress * 100) + "%" : "0%"
                    color: dialog.c.fg
                    font.pixelSize: 13
                    font.family: dialog.m.numberFamily
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 12
                radius: dialog.m.square ? 0 : 6
                color: dialog.c.track

                Rectangle {
                    width: parent.width * (dialog.job ? dialog.job.overallProgress : 0)
                    height: parent.height
                    radius: parent.radius
                    color: dialog.c.accent
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: qsTr("Current stage")
                    color: dialog.c.fg2
                    font.pixelSize: 13
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: dialog.job ? Math.round(dialog.job.stageProgress * 100) + "%" : "0%"
                    color: dialog.c.fg
                    font.pixelSize: 13
                    font.family: dialog.m.numberFamily
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 10
                radius: dialog.m.square ? 0 : 5
                color: dialog.c.track

                Rectangle {
                    width: parent.width * (dialog.job ? dialog.job.stageProgress : 0)
                    height: parent.height
                    radius: parent.radius
                    color: dialog.c.segActive
                }
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            DialogButton {
                implicitWidth: 110
                text: dialog.job && dialog.job.paused ? qsTr("Resume") : qsTr("Pause")
                enabled: dialog.job && dialog.job.running
                onClicked: dialog.job.togglePaused()
            }

            Item { Layout.fillWidth: true }

            Text {
                text: (dialog.job
                       ? qsTr("Elapsed %1   ·   Remaining %2")
                         .arg(dialog.formatTime(dialog.job.elapsedSeconds))
                         .arg(dialog.formatTime(dialog.job.etaSeconds))
                       : qsTr("Elapsed --:--   ·   Remaining --:--"))
                color: dialog.c.fg2
                font.pixelSize: 13
                font.family: dialog.m.numberFamily
                horizontalAlignment: Text.AlignHCenter
            }

            Item { Layout.fillWidth: true }

            DialogButton {
                implicitWidth: 110
                text: qsTr("Interrupt")
                danger: true
                enabled: dialog.job && dialog.job.running
                onClicked: interruptConfirm.open()
            }
        }
    }

    Popup {
        id: interruptConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 360
        modal: true
        focus: true
        padding: 18
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: dialog.m.radiusLg
            color: dialog.c.card
            border.width: 1
            border.color: dialog.c.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 14

            Text {
                text: qsTr("Interrupt this render?")
                color: dialog.c.fg
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("The renderer will stop at the next safe point. Files already written stay in the output directory.")
                color: dialog.c.fg2
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item { Layout.fillWidth: true }
                DialogButton {
                    text: qsTr("Keep rendering")
                    onClicked: interruptConfirm.close()
                }
                DialogButton {
                    text: qsTr("Interrupt")
                    danger: true
                    onClicked: {
                        interruptConfirm.close();
                        if (dialog.job) dialog.job.interrupt();
                    }
                }
            }
        }
    }
}
