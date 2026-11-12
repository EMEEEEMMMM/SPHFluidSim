import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import IISPH.Theme

Popup {
    id: dialog
    property var session
    property var job
    property var cameraSource        // Main.qml viewportProbe (optional)
    // Event state lives on the dialog so pullCurrentParams() can reset it per
    // open without touching the session's live parameters.
    property bool dropEnabled: false

    signal started()

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property var p: session ? session.params : ({})
    property var validation: ({ valid: false, message: "Checking the output path…" })
    property string actionError: ""
    property bool syncingFrameTime: false
    // Derived event warnings: property bindings so the estimate updates while
    // the radius (or particle count) changes.
    readonly property real dropCount: dialog.dropParticleCount()
    readonly property string dropBudgetMessage: dialog.dropBudgetError()
    // Defensive: Main keeps the dialog closed in Empty, but never map an empty
    // scene onto a dam render if it is opened some other way.
    readonly property bool emptyScene: session && session.isEmpty === true

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

    // QColor -> "#RRGGBB" for render.background.
    function colorToHex(col) {
        if (!col || col.r === undefined) return "#1F2638";
        function byte(v) {
            const n = Math.max(0, Math.min(255, Math.round(Number(v) * 255)));
            return (n < 16 ? "0" : "") + n.toString(16).toUpperCase();
        }
        return "#" + byte(col.r) + byte(col.g) + byte(col.b);
    }

    // Models are exported by the session; Empty is the only scene that can
    // hold them, and headless renders never run there. Keep exactly the schema
    // v2 keys (the session also publishes transform/triangleCount for the
    // renderer, which must not leak into the job config).
    function modelConfigs() {
        if (!(dialog.session && dialog.session.modelConfigs !== undefined)) return [];
        const cfgs = dialog.session.modelConfigs();
        if (!cfgs) return [];
        const models = [];
        for (let i = 0; i < cfgs.length; ++i) {
            const m = cfgs[i];
            models.push({
                path: m.path,
                position: m.position,
                rotation: m.rotation,
                scale: m.scale,
                visible: m.visible
            });
        }
        return models;
    }

    // Approximate particle count of the drop: sphere volume / spacing³.
    function dropParticleCount() {
        if (!dialog.dropEnabled) return 0;
        const spacing = dialog.p ? Number(dialog.p.spacing) : 0;
        if (!(spacing > 0)) return 0;
        const r = Math.max(0, dropRadiusField.value);
        const volume = 4.0 / 3.0 * Math.PI * r * r * r;
        return Math.max(0, Math.round(volume / Math.pow(spacing, 3)));
    }

    function dropBudgetError() {
        if (sceneField.currentIndex === 1 || !dialog.dropEnabled) return "";
        const base = Math.max(128, Math.round(particleField.value));
        if (!dialog.session || dialog.session.particleBudgetError === undefined) return "";
        const total = Math.min(2147483647, base + dialog.dropParticleCount());
        return dialog.session.particleBudgetError(total);
    }

    function formatCount(n) {
        const raw = String(Math.max(0, Math.round(n)));
        let out = "";
        let count = 0;
        for (let i = raw.length - 1; i >= 0; --i) {
            out = raw[i] + out;
            if (++count % 3 === 0 && i > 0) out = " " + out;
        }
        return out;
    }

    // Keep the live session's drop shape in sync with the event editor; the
    // headless run uses the config, the live scene uses these parameters.
    function syncDropParams() {
        if (!session || session.setDropParams === undefined) return;
        session.setDropParams(dropRadiusField.value,
                              dropXField.value,
                              dropYField.value,
                              dropZField.value);
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

        const events = [];
        // One drop event, Dam only; Inflow keeps its continuous source.
        if (!inflow && dialog.dropEnabled) {
            events.push({
                type: "drop",
                time: dropTimeField.value,
                radius: dropRadiusField.value,
                x: dropXField.value,
                y: dropYField.value,
                z: dropZField.value
            });
        }

        const cam = cameraEditor.config();

        return {
            app: "IISPH",
            schemaVersion: 2,
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
                wallAdhesion: adhesionField.value,
                models: dialog.modelConfigs()
            },
            events: events,
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
                noVideo: noVideo.checked,
                camera: {
                    yaw: cam.yaw,
                    pitch: cam.pitch,
                    distance: cam.distance,
                    target: [cam.targetX, cam.targetY, cam.targetZ],
                    fov: cam.fov
                },
                background: dialog.colorToHex(dialog.c.viewportBackground)
            }
        };
    }

    function validateNow() {
        if (!session || !job) return;
        if (dialog.emptyScene) {
            validation = ({ valid: false,
                            message: qsTr("The Empty scene supports screenshots, not offline renders.") });
            return;
        }
        const particles = Math.max(128, Math.round(particleField.value));
        const budgetError = session.particleBudgetError(particles);
        if (budgetError !== "") {
            validation = ({ valid: false, message: budgetError });
            return;
        }
        const eventError = dialog.dropBudgetError();
        if (eventError !== "") {
            validation = ({ valid: false, message: eventError });
            return;
        }
        validation = job.validateOptions(buildConfig());
    }

    // When the user switches the scene inside the dialog, restore that scene's
    // fixed target (Dam 0.55, Inflow 0.50) instead of carrying the old framing.
    function resetCameraForScene(sceneIndex) {
        if (!cameraEditor || !session) return;
        const cfg = session.cameraState;
        if (!cfg) return;
        cfg.targetY = sceneIndex === 1 ? 0.5 : 0.55;
        cameraEditor.applyConfig(cfg);
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

        // Camera defaults: the session's per-scene camera state (fixed headless
        // views). The live viewport is only copied by "Use current view".
        cameraEditor.refresh();

        // Events: start disabled every time the dialog opens; shape/position
        // come from the session's current drop parameters.
        dialog.dropEnabled = false;
        dropToggle.checked = false;
        if (session.dropParams !== undefined) {
            const dp = session.dropParams();
            if (dp) {
                if (dp.radius !== undefined) dropRadiusField.value = Number(dp.radius);
                if (dp.x !== undefined) dropXField.value = Number(dp.x);
                if (dp.y !== undefined) dropYField.value = Number(dp.y);
                if (dp.z !== undefined) dropZField.value = Number(dp.z);
            }
        }
        dropTimeField.value = 1.0;

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
                            onActivated: {
                                dialog.resetCameraForScene(sceneField.currentIndex);
                                dialog.scheduleValidation();
                            }
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
                            to: 2147483647
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
                    heading: qsTr("Events")
                    // Dam break only: Inflow has a continuous source instead of
                    // a timed drop.
                    visible: sceneField.currentIndex === 0

                    RenderToggle {
                        id: dropToggle
                        label: qsTr("Spawn a water drop")
                        checked: false
                        onToggled: {
                            dialog.dropEnabled = checked;
                            dialog.scheduleValidation();
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 22
                        rowSpacing: 8
                        visible: dialog.dropEnabled

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            RenderField {
                                id: dropTimeField
                                Layout.fillWidth: true
                                label: qsTr("Drop time")
                                suffix: "s"
                                value: 1
                                from: 0
                                to: 100000
                                decimals: 3
                                onEdited: dialog.scheduleValidation()
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("frame ")
                                      + Math.round(dropTimeField.value
                                                   * Math.max(1, Math.round(fpsField.value)))
                                color: dialog.c.fg3
                                font.pixelSize: 10
                                font.family: dialog.m.numberFamily
                            }
                        }
                        RenderField {
                            id: dropRadiusField
                            Layout.fillWidth: true
                            label: qsTr("Drop radius")
                            value: 0.2
                            from: 0.001
                            to: 100
                            decimals: 3
                            onEdited: {
                                dialog.syncDropParams();
                                dialog.scheduleValidation();
                            }
                        }
                        RenderField {
                            id: dropXField
                            Layout.fillWidth: true
                            label: qsTr("Drop X")
                            value: 1.0
                            from: -100000
                            to: 100000
                            decimals: 3
                            onEdited: {
                                dialog.syncDropParams();
                                dialog.scheduleValidation();
                            }
                        }
                        RenderField {
                            id: dropYField
                            Layout.fillWidth: true
                            label: qsTr("Drop Y")
                            value: 1.6
                            from: -100000
                            to: 100000
                            decimals: 3
                            onEdited: {
                                dialog.syncDropParams();
                                dialog.scheduleValidation();
                            }
                        }
                        RenderField {
                            id: dropZField
                            Layout.fillWidth: true
                            label: qsTr("Drop Z")
                            value: 0.5
                            from: -100000
                            to: 100000
                            decimals: 3
                            onEdited: {
                                dialog.syncDropParams();
                                dialog.scheduleValidation();
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: dialog.dropEnabled
                        text: dialog.dropBudgetMessage !== ""
                              ? dialog.dropBudgetMessage
                              : qsTr("Drop ≈ ") + dialog.formatCount(dialog.dropCount)
                                + qsTr(" particles  ·  total ≈ ")
                                + dialog.formatCount(Math.max(128, Math.round(particleField.value))
                                                     + dialog.dropCount)
                        color: dialog.dropBudgetMessage !== "" ? dialog.c.danger : dialog.c.fg3
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                }

                RenderSection {
                    heading: qsTr("Camera")

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Fixed per-scene view by default. \"Use current view\" copies the live viewport.")
                            color: dialog.c.fg3
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }

                        CameraSection {
                            id: cameraEditor
                            Layout.fillWidth: true
                            session: dialog.session
                            cameraSource: dialog.cameraSource
                            live: false
                            showFields: true
                            showActions: true
                            onConfigChanged: dialog.scheduleValidation()
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
                         && !dialog.emptyScene
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
