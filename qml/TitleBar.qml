import QtQuick
import QtQuick.Layouts
import IISPH.Theme

Rectangle {
    id: bar
    property var session
    signal viewModeRequested(int mode)
    signal screenshotRequested()
    signal fullscreenRequested()

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property bool running: session ? session.running : false
    readonly property var p: session ? session.params : ({})

    implicitHeight: 52
    color: c.chrome

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: bar.c.border
    }

    MouseArea {
        anchors.fill: parent
        z: 0
        onPressed: Window.window.startSystemMove()
        onDoubleClicked: {
            if (Window.window.visibility === Window.Maximized)
                Window.window.showNormal();
            else
                Window.window.showMaximized();
        }
    }

    // Brand stays at the far left; the transport below is independently anchored
    // to the window centre so the play/pause button is always exactly centred.
    RowLayout {
        id: brand
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        z: 1
        spacing: 10

        Rectangle {
            width: 26
            height: 26
            radius: bar.m.square ? 0 : 7
            color: bar.c.accent
            Text {
                anchors.centerIn: parent
                text: "SPH"
                color: "#FFFFFF"
                font.pixelSize: 11
                font.weight: Font.Bold
            }
        }
        ColumnLayout {
            spacing: 0
            Text {
                text: "SPHFluidSim"
                color: bar.c.fg
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Text {
                text: "Implicit Incompressible SPH"
                color: bar.c.fg3
                font.pixelSize: 10
            }
        }
    }

    IconBtn {
        id: resetButton
        anchors.right: playButton.left
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        z: 1
        kind: 3
        onClicked: bar.session.resetSimulation()
    }

    IconBtn {
        id: playButton
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        z: 1
        kind: bar.running ? 1 : 0
        circular: true
        primary: true
        implicitWidth: 32
        implicitHeight: 32
        onClicked: bar.session.toggleRunning()
    }

    IconBtn {
        id: stepButton
        anchors.left: playButton.right
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        z: 1
        kind: 2
        onClicked: bar.session.stepOnce()
    }

    Rectangle {
        id: clock
        anchors.left: stepButton.right
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        z: 1
        implicitWidth: clockLabel.implicitWidth + 18
        implicitHeight: 22
        radius: bar.m.square ? 0 : 9
        color: bar.c.chip
        border.width: 1
        border.color: bar.c.border

        Text {
            id: clockLabel
            anchors.centerIn: parent
            text: "t " + Number(bar.session && bar.session.status ? bar.session.status.simTime : 0).toFixed(2) + " s"
            color: bar.c.fg2
            font.pixelSize: 10
            font.family: bar.m.numberFamily
        }
    }

    RowLayout {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        z: 1
        spacing: 8

        Segmented {
            options: ["Particles", "Surface", "Both"]
            currentIndex: bar.p.viewMode !== undefined ? bar.p.viewMode : 0
            onActivated: (index) => bar.viewModeRequested(index)
        }

        IconBtn { kind: 7; onClicked: bar.screenshotRequested() }
        IconBtn { kind: 5; onClicked: bar.fullscreenRequested() }

        Rectangle {
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            width: 1
            height: 18
            color: bar.c.border
        }

        WindowButton {
            kind: 10
            onClicked: Window.window.showMinimized()
        }
        WindowButton {
            kind: 11
            onClicked: {
                if (Window.window.visibility === Window.Maximized)
                    Window.window.showNormal();
                else
                    Window.window.showMaximized();
            }
        }
        WindowButton {
            kind: 12
            danger: true
            onClicked: Window.window.close()
        }
    }
}
