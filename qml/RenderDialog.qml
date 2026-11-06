import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import IISPH.Theme

Popup {
    id: dialog
    property var session
    property var job

    signal started()

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property var p: session ? session.params : ({})
    property var validation: ({ valid: false, message: "Checking the output path…" })
    property string actionError: ""
    property bool syncingFrameTime: false

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(860, parent ? parent.width - 64 : 860)
    height: Math.min(900, parent ? parent.height - 24 : 900)
    modal: true
    focus: true
    padding: 18
    closePolicy: Popup.CloseOnEscape

    function scheduleValidation() {
        actionError = "";
        validationTimer.restart();
    }

    function framesFromTime() {
        if (syncingFrameTime) return;
        const fps = Math.max(1, Math.round(fpsField.value));
        const frames = Math.max(1, Math.min(1000000, Math.round(timeField.value * fps)));
        syncingFrameTime = true;
        framesField.value = frames;
        syncingFrameTime = false;
    }

    function timeFromFrames() {
        if (syncingFrameTime) return;
        const fps = Math.max(1, Math.round(fpsField.value));
        syncingFrameTime = true;
        timeField.value = Math.max(1, Math.round(framesField.value)) / fps;
        syncingFrameTime = false;
    }

    function buildConfig() {
        const scene = sceneField.currentIndex === 1 ? "inflow" : "dam";
        const solver = solverField.currentIndex === 1 ? "DFSPH" : "IISPH";
        const mode = modeField.currentIndex === 1 ? "surface" : "particles";
        const inflow = scene === "inflow";
        const volume = inflow ? 0.6 : 0.512;
        const particles = Math.max(128, Math.round(particleField.value));
        const baseCount = inflow ? Math.max(1, particles - 8) : particles;
        const spacing = Math.cbrt(volume / baseCount);
        return {
            app: "IISPH",
            schemaVersion: 1,
            simulation: {
                scene: scene,
                solver: solver,
                particleCount: particles,
                spacing: spacing,
                dt: dtField.value,
                useCFL: cflToggle.checked,
                omega: omegaField.value,
                eta: etaField.value,
                etaV: etaVField.value,
                maxIter: maxIterField.value,
                viscosity: viscosityField.value,
                surfaceTension: tensionField.value,
                wallAdhesion: adhesionField.value
            },
            events: [],
            render: {
                mode: mode,
                gridScale: gridField.value,
                surfaceThreshold: thresholdField.value,
                reconInterval: reconField.value,
                fps: fpsField.value,
                timeScale: timeScaleField.value,
                width: widthField.value,
                height: heightField.value,
                frames: framesField.value,
                threads: threadsField.value,
                exportParticles: exportPly.checked,
                output: outputField.text.trim(),
                keepFrames: keepFrames.checked,
                noVideo: noVideo.checked
            }
        };
    }

    function validateNow() {
        if (!session || !job) return;
        validation = job.validateOptions(buildConfig());
    }

    function pullCurrentParams() {
        if (!session) return;
        const current = session.params;
        sceneField.currentIndex = current.scene === 1 ? 1 : 0;
        solverField.currentIndex = current.solver === 1 ? 1 : 0;
        modeField.currentIndex = current.viewMode === 1 ? 1 : 0;
        const currentSpacing = Math.max(1.0e-4, Number(current.spacing));
        const currentVolume = current.scene === 1 ? 0.6 : 0.512;
        particleField.value = Math.round(currentVolume / Math.pow(currentSpacing, 3))
                              + (current.scene === 1 ? 8 : 0);
        dtField.value = Number(current.dt);
        cflToggle.checked = current.cfl === true;
        omegaField.value = Number(current.omega);
        etaField.value = Number(current.eta);
        etaVField.value = Number(current.etaV);
        maxIterField.value = Number(current.maxIter);
        viscosityField.value = Number(current.viscosity);
        tensionField.value = Number(current.surfaceTension);
        adhesionField.value = Number(current.wallAdhesion);
        thresholdField.value = Number(current.surfaceThreshold);
        gridField.value = Number(current.gridScale);
        reconField.value = Number(current.reconInterval);
        syncingFrameTime = true;
        fpsField.value = 30;
        framesField.value = 300;
        timeField.value = 10;
        syncingFrameTime = false;
        outputField.text = job ? job.defaultOutputPath() : "";
        actionError = "";
        validateNow();
    }

    onOpened: pullCurrentParams()

    background: Rectangle {
        radius: dialog.m.radiusLg
        color: dialog.c.card
        border.width: 1
        border.color: dialog.c.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Text {
                    text: qsTr("Render")
                    color: dialog.c.fg
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Offline headless render · the live simulation is left untouched")
                    color: dialog.c.fg3
                    font.pixelSize: 12
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: dialog.c.border
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            ColumnLayout {
                width: scroll.availableWidth
                spacing: 10

                RenderSection {
                    heading: qsTr("Simulation")

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 22
                        rowSpacing: 8

                        RenderSegmented {
                            id: sceneField
                            Layout.fillWidth: true
                            label: qsTr("Scene")
                            options: [qsTr("Dam break"), qsTr("Inflow")]
                            onActivated: dialog.scheduleValidation()
                        }
                        RenderSegmented {
                            id: solverField
                            Layout.fillWidth: true
                            label: qsTr("Solver")
                            options: ["IISPH", "DFSPH"]
                            onActivated: dialog.scheduleValidation()
                        }
                        RenderSegmented {
                            id: modeField
                            Layout.fillWidth: true
                            label: qsTr("Render mode")
                            options: [qsTr("Particles"), qsTr("Surface")]
                            onActivated: dialog.scheduleValidation()
                        }
                        Item { Layout.fillWidth: true; Layout.preferredHeight: 1 }
                    }
                }

                RenderSection {
                    heading: qsTr("Fluid parameters")

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 22
                        rowSpacing: 8

                        RenderField {
                            id: particleField
                            Layout.fillWidth: true
                            label: qsTr("Number of particles")
                            value: 32768
                            from: 128
                            to: 2000000
                            integer: true
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: dtField
                            Layout.fillWidth: true
                            label: qsTr("Time step")
                            suffix: "s"
                            value: 0.004
                            from: 0
                            to: 0.05
                            decimals: 5
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: omegaField
                            Layout.fillWidth: true
                            label: qsTr("Omega")
                            value: 0.5
                            from: 0
                            to: 2
                            decimals: 3
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: etaField
                            Layout.fillWidth: true
                            label: qsTr("Eta")
                            value: 0.01
                            from: 0
                            to: 2
                            decimals: 4
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: etaVField
                            Layout.fillWidth: true
                            label: qsTr("Eta viscosity")
                            value: 0.01
                            from: 0
                            to: 2
                            decimals: 4
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: maxIterField
                            Layout.fillWidth: true
                            label: qsTr("Max iterations")
                            value: 100
                            from: 1
                            to: 1000
                            integer: true
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: viscosityField
                            Layout.fillWidth: true
                            label: qsTr("Viscosity")
                            value: 0.01
                            from: 0
                            to: 1
                            decimals: 4
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: tensionField
                            Layout.fillWidth: true
                            label: qsTr("Surface tension")
                            value: 0.02
                            from: 0
                            to: 10
                            decimals: 4
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: adhesionField
                            Layout.fillWidth: true
                            label: qsTr("Wall adhesion")
                            value: 0
                            from: 0
                            to: 10
                            decimals: 4
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderToggle {
                            id: cflToggle
                            Layout.fillWidth: true
                            label: qsTr("Adaptive CFL step")
                            checked: true
                            onToggled: dialog.scheduleValidation()
                        }
                    }
                }

                RenderSection {
                    heading: qsTr("Surface")
                    visible: modeField.currentIndex === 1

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 22
                        rowSpacing: 8

                        RenderField {
                            id: gridField
                            Layout.fillWidth: true
                            label: qsTr("Grid scale")
                            value: 0.5
                            from: 0.1
                            to: 4
                            decimals: 3
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: thresholdField
                            Layout.fillWidth: true
                            label: qsTr("Surface threshold")
                            value: 0.8
                            from: 0
                            to: 4
                            decimals: 3
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: reconField
                            Layout.fillWidth: true
                            label: qsTr("Rebuild interval")
                            suffix: qsTr("frames")
                            value: 3
                            from: 1
                            to: 100
                            integer: true
                            onEdited: dialog.scheduleValidation()
                        }
                    }
                }

                RenderSection {
                    heading: qsTr("Scheduling")

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 22
                        rowSpacing: 8

                        RenderField {
                            id: fpsField
                            Layout.fillWidth: true
                            label: qsTr("Frames per second")
                            value: 30
                            from: 1
                            to: 240
                            integer: true
                            onEdited: {
                                dialog.scheduleValidation();
                                dialog.framesFromTime();
                            }
                        }
                        RenderField {
                            id: timeScaleField
                            Layout.fillWidth: true
                            label: qsTr("Time scale")
                            value: 1
                            from: 0.001
                            to: 100
                            decimals: 3
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: timeField
                            Layout.fillWidth: true
                            label: qsTr("Time")
                            suffix: "s"
                            value: 10
                            from: 0.001
                            to: 100000
                            decimals: 3
                            onEdited: {
                                dialog.scheduleValidation();
                                dialog.framesFromTime();
                            }
                        }
                        RenderField {
                            id: framesField
                            Layout.fillWidth: true
                            label: qsTr("Frames")
                            value: 300
                            from: 1
                            to: 1000000
                            integer: true
                            onEdited: {
                                dialog.scheduleValidation();
                                dialog.timeFromFrames();
                            }
                        }
                        RenderField {
                            id: widthField
                            Layout.fillWidth: true
                            label: qsTr("Width")
                            suffix: "px"
                            value: 1280
                            from: 64
                            to: 16384
                            integer: true
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: heightField
                            Layout.fillWidth: true
                            label: qsTr("Height")
                            suffix: "px"
                            value: 720
                            from: 64
                            to: 16384
                            integer: true
                            onEdited: dialog.scheduleValidation()
                        }
                        RenderField {
                            id: threadsField
                            Layout.fillWidth: true
                            label: qsTr("CPU threads (0 = all)")
                            value: 0
                            from: 0
                            to: 256
                            integer: true
                            onEdited: dialog.scheduleValidation()
                        }
                    }
                }

            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: dialog.c.border
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 7

            Text {
                text: qsTr("OUTPUT")
                color: dialog.c.fg3
                font.pixelSize: 10
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    radius: dialog.m.square ? 0 : dialog.m.radiusSm
                    color: dialog.c.field
                    border.width: outputField.activeFocus ? 2 : 1
                    border.color: outputField.activeFocus ? dialog.c.accent : dialog.c.border

                    TextField {
                        id: outputField
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        padding: 0
                        color: dialog.c.fg
                        placeholderText: qsTr("Choose where frames, PLY files and the video are written")
                        placeholderTextColor: dialog.c.fg3
                        selectionColor: dialog.c.accent
                        selectedTextColor: dialog.c.accentFg
                        font.pixelSize: 12
                        verticalAlignment: Text.AlignVCenter
                        selectByMouse: true
                        onTextEdited: dialog.scheduleValidation()
                        background: Item {}
                    }
                }

                DialogButton {
                    implicitWidth: 84
                    text: qsTr("Browse")
                    onClicked: {
                        if (outputField.text.length > 0) folderDialog.currentFolder = "file:///" + outputField.text.replace(/\\/g, "/");
                        folderDialog.open();
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 24

                RenderToggle {
                    id: exportPly
                    label: qsTr("Export particle PLY")
                    onToggled: dialog.scheduleValidation()
                }
                RenderToggle {
                    id: keepFrames
                    label: qsTr("Keep BMP frames")
                    onToggled: dialog.scheduleValidation()
                }
                RenderToggle {
                    id: noVideo
                    label: qsTr("Skip video")
                    onToggled: dialog.scheduleValidation()
                }
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                text: (dialog.validation.valid ? qsTr("Ready — ") : qsTr("Not ready — "))
                      + (dialog.validation.message || "")
                color: dialog.validation.valid ? dialog.c.ok : dialog.c.danger
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: dialog.c.border
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: dialog.actionError !== ""
                      ? dialog.actionError
                      : (dialog.job && dialog.job.running ? qsTr("A render is already running.") : "")
                color: dialog.c.danger
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                visible: text !== ""
            }

            DialogButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }

            DialogButton {
                text: qsTr("Start")
                primary: true
                enabled: dialog.validation.valid && dialog.job && !dialog.job.running
                onClicked: {
                    const config = dialog.buildConfig();
                    dialog.validation = dialog.job.validateOptions(config);
                    if (!dialog.validation.valid) {
                        dialog.actionError = dialog.validation.message;
                        return;
                    }
                    if (dialog.job.start(config)) {
                        dialog.started();
                        dialog.close();
                    } else {
                        dialog.actionError = qsTr("Could not start the render process.");
                    }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose render output directory")
        onAccepted: {
            outputField.text = dialog.job.urlToLocalPath(selectedFolder);
            dialog.scheduleValidation();
        }
    }

    Timer {
        id: validationTimer
        interval: 160
        onTriggered: dialog.validateNow()
    }
}
