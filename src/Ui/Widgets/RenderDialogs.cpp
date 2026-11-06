#include "Ui/Widgets/RenderDialogs.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QGuiApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantList>

#include <cmath>

#include "Render/RenderJob.hpp"
#include "Session/SimulationSession.hpp"
#include "Ui/Theme.hpp"

namespace ui {
namespace {

QLabel* Caption(const QString& text) {
    auto* label = new QLabel(text);
    label->setProperty("role", "caption");
    return label;
}

void AddField(QGridLayout* grid, int row, int column, const QString& label, QWidget* field) {
    grid->addWidget(Caption(label), row, column);
    grid->addWidget(field, row, column + 1);
}

QString FormatDuration(double seconds) {
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) return QStringLiteral("--:--");
    const qint64 total = static_cast<qint64>(seconds + 0.5);
    const qint64 h = total / 3600;
    const qint64 m = (total % 3600) / 60;
    const qint64 s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

}  // namespace

RenderOptionsDialog::RenderOptionsDialog(SimulationSession* session, RenderJob* job, QWidget* parent)
    : QDialog(parent), m_session(session), m_job(job) {
    setWindowTitle(QStringLiteral("Render"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setModal(true);
    setMinimumSize(760, 560);
    QScreen* targetScreen = screen();
    if (!targetScreen) targetScreen = QGuiApplication::primaryScreen();
    const QRect available = targetScreen ? targetScreen->availableGeometry() : QRect(0, 0, 1440, 900);
    resize(std::min(920, available.width() - 60), std::min(880, available.height() - 80));
    buildUi();
    setStyleSheet(QStringLiteral(
        "QLabel[role=\"caption\"] { font-size: 12px; }"
        "QGroupBox { font-size: 12px; }"
        "QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox, QCheckBox { font-size: 12px; }"
        "QPushButton { font-size: 12px; padding: 6px 12px; }"));
    loadValues();

    m_validateTimer = new QTimer(this);
    m_validateTimer->setSingleShot(true);
    m_validateTimer->setInterval(140);
    connect(m_validateTimer, &QTimer::timeout, this, &RenderOptionsDialog::updateValidation);
    connect(&Theme::instance(), &Theme::changed, this, &RenderOptionsDialog::updateValidation);
    updateValidation();
}

void RenderOptionsDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("Render"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 5.0);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    root->addWidget(title);

    auto* subtitle = Caption(
        QStringLiteral("Offline headless render. The live simulation keeps running independently."));
    root->addWidget(subtitle);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("RenderOptionsPage"));
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 2, 6, 2);
    pageLayout->setSpacing(10);
    scroll->setWidget(page);
    root->addWidget(scroll, 1);

    const auto makeGroup = [page](const QString& name, QGridLayout** outGrid) {
        auto* group = new QGroupBox(name, page);
        auto* grid = new QGridLayout(group);
        grid->setContentsMargins(12, 16, 12, 10);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(8);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
        page->layout()->addWidget(group);
        *outGrid = grid;
        return group;
    };

    QGridLayout* simulationGrid = nullptr;
    makeGroup(QStringLiteral("SIMULATION"), &simulationGrid);
    m_scene = new QComboBox(this);
    m_scene->addItems({QStringLiteral("Dam break"), QStringLiteral("Inflow")});
    m_solver = new QComboBox(this);
    m_solver->addItems({QStringLiteral("IISPH"), QStringLiteral("DFSPH")});
    m_mode = new QComboBox(this);
    m_mode->addItems({QStringLiteral("Particles"), QStringLiteral("Surface")});
    m_cfl = new QCheckBox(QStringLiteral("Adaptive CFL step"), this);
    AddField(simulationGrid, 0, 0, QStringLiteral("Scene"), m_scene);
    AddField(simulationGrid, 0, 2, QStringLiteral("Solver"), m_solver);
    AddField(simulationGrid, 1, 0, QStringLiteral("Render mode"), m_mode);
    simulationGrid->addWidget(m_cfl, 1, 2, 1, 2);

    QGridLayout* fluidGrid = nullptr;
    makeGroup(QStringLiteral("FLUID PARAMETERS"), &fluidGrid);
    m_particleCount = new QSpinBox(this);
    m_particleCount->setRange(128, 2000000);
    m_particleCount->setSingleStep(1024);
    m_particleCount->setKeyboardTracking(false);
    m_dt = new QDoubleSpinBox(this);
    m_dt->setRange(0.0, 0.1);
    m_dt->setDecimals(5);
    m_dt->setSingleStep(0.0005);
    m_dt->setSuffix(QStringLiteral(" s"));
    m_omega = new QDoubleSpinBox(this);
    m_omega->setRange(0.0, 2.0);
    m_omega->setDecimals(3);
    m_omega->setSingleStep(0.05);
    m_eta = new QDoubleSpinBox(this);
    m_eta->setRange(0.0, 2.0);
    m_eta->setDecimals(4);
    m_eta->setSingleStep(0.001);
    m_etaV = new QDoubleSpinBox(this);
    m_etaV->setRange(0.0, 2.0);
    m_etaV->setDecimals(4);
    m_etaV->setSingleStep(0.001);
    m_maxIter = new QSpinBox(this);
    m_maxIter->setRange(1, 1000);
    m_viscosity = new QDoubleSpinBox(this);
    m_viscosity->setRange(0.0, 1.0);
    m_viscosity->setDecimals(4);
    m_viscosity->setSingleStep(0.001);
    m_tension = new QDoubleSpinBox(this);
    m_tension->setRange(0.0, 10.0);
    m_tension->setDecimals(4);
    m_tension->setSingleStep(0.005);
    m_adhesion = new QDoubleSpinBox(this);
    m_adhesion->setRange(0.0, 10.0);
    m_adhesion->setDecimals(4);
    m_adhesion->setSingleStep(0.005);
    AddField(fluidGrid, 0, 0, QStringLiteral("Number of particles"), m_particleCount);
    AddField(fluidGrid, 0, 2, QStringLiteral("Time step (0 = CFL)"), m_dt);
    AddField(fluidGrid, 1, 0, QStringLiteral("Omega"), m_omega);
    AddField(fluidGrid, 1, 2, QStringLiteral("Eta"), m_eta);
    AddField(fluidGrid, 2, 0, QStringLiteral("Eta viscosity"), m_etaV);
    AddField(fluidGrid, 2, 2, QStringLiteral("Max iterations"), m_maxIter);
    AddField(fluidGrid, 3, 0, QStringLiteral("Viscosity"), m_viscosity);
    AddField(fluidGrid, 3, 2, QStringLiteral("Surface tension"), m_tension);
    AddField(fluidGrid, 4, 0, QStringLiteral("Wall adhesion"), m_adhesion);

    QGridLayout* surfaceGrid = nullptr;
    m_surfaceGroup = makeGroup(QStringLiteral("SURFACE"), &surfaceGrid);
    m_grid = new QDoubleSpinBox(this);
    m_grid->setRange(0.05, 4.0);
    m_grid->setDecimals(3);
    m_grid->setSingleStep(0.05);
    m_threshold = new QDoubleSpinBox(this);
    m_threshold->setRange(0.0, 4.0);
    m_threshold->setDecimals(3);
    m_threshold->setSingleStep(0.05);
    m_recon = new QSpinBox(this);
    m_recon->setRange(1, 1000);
    m_recon->setSuffix(QStringLiteral(" frames"));
    AddField(surfaceGrid, 0, 0, QStringLiteral("Grid scale"), m_grid);
    AddField(surfaceGrid, 0, 2, QStringLiteral("Surface threshold"), m_threshold);
    AddField(surfaceGrid, 1, 0, QStringLiteral("Rebuild interval"), m_recon);

    QGridLayout* schedulingGrid = nullptr;
    makeGroup(QStringLiteral("SCHEDULING"), &schedulingGrid);
    m_fps = new QSpinBox(this);
    m_fps->setRange(1, 240);
    m_fps->setSuffix(QStringLiteral(" fps"));
    m_fps->setKeyboardTracking(false);
    m_timeScale = new QDoubleSpinBox(this);
    m_timeScale->setRange(0.001, 100.0);
    m_timeScale->setDecimals(3);
    m_timeScale->setSingleStep(0.1);
    m_timeSeconds = new QDoubleSpinBox(this);
    m_timeSeconds->setRange(0.001, 100000.0);
    m_timeSeconds->setDecimals(3);
    m_timeSeconds->setSingleStep(0.1);
    m_timeSeconds->setSuffix(QStringLiteral(" s"));
    m_timeSeconds->setKeyboardTracking(false);
    m_width = new QSpinBox(this);
    m_width->setRange(16, 16384);
    m_width->setSuffix(QStringLiteral(" px"));
    m_height = new QSpinBox(this);
    m_height->setRange(16, 16384);
    m_height->setSuffix(QStringLiteral(" px"));
    m_frames = new QSpinBox(this);
    m_frames->setRange(1, 1000000);
    m_frames->setKeyboardTracking(false);
    m_threads = new QSpinBox(this);
    m_threads->setRange(0, 256);
    AddField(schedulingGrid, 0, 0, QStringLiteral("Frame rate"), m_fps);
    AddField(schedulingGrid, 0, 2, QStringLiteral("Time scale"), m_timeScale);
    AddField(schedulingGrid, 1, 0, QStringLiteral("Time"), m_timeSeconds);
    AddField(schedulingGrid, 1, 2, QStringLiteral("Frames"), m_frames);
    AddField(schedulingGrid, 2, 0, QStringLiteral("Width"), m_width);
    AddField(schedulingGrid, 2, 2, QStringLiteral("Height"), m_height);
    AddField(schedulingGrid, 3, 0, QStringLiteral("CPU threads (0 = all)"), m_threads);

    auto* outputGroup = new QGroupBox(QStringLiteral("OUTPUT"), this);
    auto* outputGrid = new QGridLayout(outputGroup);
    outputGrid->setContentsMargins(12, 16, 12, 10);
    outputGrid->setHorizontalSpacing(10);
    outputGrid->setVerticalSpacing(6);
    auto* outputRow = new QWidget(outputGroup);
    auto* outputLayout = new QHBoxLayout(outputRow);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->setSpacing(8);
    m_output = new QLineEdit(outputRow);
    m_output->setPlaceholderText(QStringLiteral("Directory for frames, PLY files and the video"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), outputRow);
    outputLayout->addWidget(m_output, 1);
    outputLayout->addWidget(browse);
    outputGrid->addWidget(Caption(QStringLiteral("Directory")), 0, 0);
    outputGrid->addWidget(outputRow, 0, 1, 1, 3);

    auto* optionsRow = new QWidget(this);
    auto* optionsLayout = new QHBoxLayout(optionsRow);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->setSpacing(18);
    m_exportPly = new QCheckBox(QStringLiteral("Export particle PLY"), optionsRow);
    m_keepFrames = new QCheckBox(QStringLiteral("Keep BMP frames"), optionsRow);
    m_noVideo = new QCheckBox(QStringLiteral("Skip video encoding"), optionsRow);
    optionsLayout->addWidget(m_exportPly);
    optionsLayout->addWidget(m_keepFrames);
    optionsLayout->addWidget(m_noVideo);
    optionsLayout->addStretch(1);
    outputGrid->addWidget(optionsRow, 1, 0, 1, 4);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setProperty("role", "caption");
    outputGrid->addWidget(m_status, 2, 0, 1, 4);
    root->addWidget(outputGroup);

    pageLayout->addStretch(1);

    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    footer->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    cancel->setProperty("variant", "ghost");
    m_startButton = new QPushButton(QStringLiteral("Start"), this);
    m_startButton->setProperty("variant", "primary");
    m_startButton->setDefault(true);
    m_startButton->setEnabled(false);
    footer->addWidget(cancel);
    footer->addWidget(m_startButton);
    root->addLayout(footer);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, &RenderOptionsDialog::chooseOutputDirectory);
    connect(m_startButton, &QPushButton::clicked, this, &RenderOptionsDialog::startRender);

    connect(m_scene, &QComboBox::currentIndexChanged, this,
            [this](int) { scheduleValidation(); });
    connect(m_solver, &QComboBox::currentIndexChanged, this,
            [this](int) { scheduleValidation(); });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int) {
        scheduleValidation();
        updateSurfaceVisibility();
    });
    for (QSpinBox* spin : findChildren<QSpinBox*>())
        connect(spin, &QSpinBox::valueChanged, this, [this](int) { scheduleValidation(); });
    for (QDoubleSpinBox* spin : findChildren<QDoubleSpinBox*>())
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double) { scheduleValidation(); });
    for (QCheckBox* check : findChildren<QCheckBox*>())
        connect(check, &QCheckBox::toggled, this, [this](bool) { scheduleValidation(); });
    connect(m_output, &QLineEdit::textChanged, this, [this](const QString&) { scheduleValidation(); });
    connect(m_fps, &QSpinBox::valueChanged, this, [this](int) { framesFromTime(); });
    connect(m_timeSeconds, &QDoubleSpinBox::valueChanged, this,
            [this](double) { framesFromTime(); });
    connect(m_frames, &QSpinBox::valueChanged, this, [this](int) { timeFromFrames(); });
}

void RenderOptionsDialog::loadValues() {
    const QVariantMap p = m_session ? m_session->params() : QVariantMap{};
    m_scene->setCurrentIndex(p.value(QStringLiteral("scene")).toInt() == 1 ? 1 : 0);
    m_solver->setCurrentIndex(p.value(QStringLiteral("solver")).toInt() == 1 ? 1 : 0);
    m_mode->setCurrentIndex(p.value(QStringLiteral("viewMode")).toInt() == 1 ? 1 : 0);
    const bool currentInflow = p.value(QStringLiteral("scene")).toInt() == 1;
    const double currentSpacing =
        std::max(0.0001, p.value(QStringLiteral("spacing"), 0.025).toDouble());
    const double currentVolume = currentInflow ? 0.6 : 0.512;
    m_particleCount->setValue(
        std::clamp(qRound(currentVolume / std::pow(currentSpacing, 3.0)) +
                       (currentInflow ? 8 : 0),
                   128, 2000000));
    m_dt->setValue(p.value(QStringLiteral("dt"), 0.004).toDouble());
    m_cfl->setChecked(p.value(QStringLiteral("cfl"), true).toBool());
    m_omega->setValue(p.value(QStringLiteral("omega"), 0.5).toDouble());
    m_eta->setValue(p.value(QStringLiteral("eta"), 0.01).toDouble());
    m_etaV->setValue(p.value(QStringLiteral("etaV"), 0.01).toDouble());
    m_maxIter->setValue(p.value(QStringLiteral("maxIter"), 100).toInt());
    m_viscosity->setValue(p.value(QStringLiteral("viscosity"), 0.01).toDouble());
    m_tension->setValue(p.value(QStringLiteral("surfaceTension"), 0.02).toDouble());
    m_adhesion->setValue(p.value(QStringLiteral("wallAdhesion"), 0.0).toDouble());
    m_grid->setValue(p.value(QStringLiteral("gridScale"), 0.5).toDouble());
    m_threshold->setValue(p.value(QStringLiteral("surfaceThreshold"), 0.8).toDouble());
    m_recon->setValue(p.value(QStringLiteral("reconInterval"), 3).toInt());
    {
        const QSignalBlocker blockFps(m_fps);
        const QSignalBlocker blockFrames(m_frames);
        const QSignalBlocker blockTime(m_timeSeconds);
        m_fps->setValue(30);
        m_frames->setValue(300);
        m_timeSeconds->setValue(10.0);
    }
    m_timeScale->setValue(1.0);
    m_width->setValue(1280);
    m_height->setValue(720);
    m_threads->setValue(0);
    updateSurfaceVisibility();
    m_exportPly->setChecked(false);
    m_keepFrames->setChecked(false);
    m_noVideo->setChecked(false);
    m_output->setText(m_job ? QDir::toNativeSeparators(m_job->defaultOutputPath()) : QString());
}

QVariantMap RenderOptionsDialog::buildConfig() const {
    const bool inflow = m_scene->currentIndex() == 1;
    const QString scene = inflow ? QStringLiteral("inflow") : QStringLiteral("dam");
    const QString solver = m_solver->currentIndex() == 1 ? QStringLiteral("DFSPH")
                                                         : QStringLiteral("IISPH");
    const QString mode = m_mode->currentIndex() == 1 ? QStringLiteral("surface")
                                                      : QStringLiteral("particles");
    const int particles = std::max(128, m_particleCount->value());
    const double volume = inflow ? 0.6 : 0.512;
    const int baseCount = inflow ? std::max(1, particles - 8) : particles;
    const double spacing = std::cbrt(volume / static_cast<double>(baseCount));

    QVariantMap simulation;
    simulation.insert(QStringLiteral("scene"), scene);
    simulation.insert(QStringLiteral("solver"), solver);
    simulation.insert(QStringLiteral("particleCount"), particles);
    simulation.insert(QStringLiteral("spacing"), spacing);
    simulation.insert(QStringLiteral("dt"), m_dt->value());
    simulation.insert(QStringLiteral("useCFL"), m_cfl->isChecked());
    simulation.insert(QStringLiteral("omega"), m_omega->value());
    simulation.insert(QStringLiteral("eta"), m_eta->value());
    simulation.insert(QStringLiteral("etaV"), m_etaV->value());
    simulation.insert(QStringLiteral("maxIter"), m_maxIter->value());
    simulation.insert(QStringLiteral("viscosity"), m_viscosity->value());
    simulation.insert(QStringLiteral("surfaceTension"), m_tension->value());
    simulation.insert(QStringLiteral("wallAdhesion"), m_adhesion->value());

    QVariantMap render;
    render.insert(QStringLiteral("mode"), mode);
    render.insert(QStringLiteral("gridScale"), m_grid->value());
    render.insert(QStringLiteral("surfaceThreshold"), m_threshold->value());
    render.insert(QStringLiteral("reconInterval"), m_recon->value());
    render.insert(QStringLiteral("fps"), m_fps->value());
    render.insert(QStringLiteral("timeScale"), m_timeScale->value());
    render.insert(QStringLiteral("width"), m_width->value());
    render.insert(QStringLiteral("height"), m_height->value());
    render.insert(QStringLiteral("frames"), m_frames->value());
    render.insert(QStringLiteral("threads"), m_threads->value());
    render.insert(QStringLiteral("exportParticles"), m_exportPly->isChecked());
    render.insert(QStringLiteral("output"),
                  QDir::fromNativeSeparators(m_output->text().trimmed()));
    render.insert(QStringLiteral("keepFrames"), m_keepFrames->isChecked());
    render.insert(QStringLiteral("noVideo"), m_noVideo->isChecked());

    QVariantMap root;
    root.insert(QStringLiteral("app"), QStringLiteral("IISPH"));
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("simulation"), simulation);
    root.insert(QStringLiteral("events"), QVariantList{});
    root.insert(QStringLiteral("render"), render);
    return root;
}

void RenderOptionsDialog::scheduleValidation() {
    if (!m_validateTimer) return;
    if (m_startButton) m_startButton->setEnabled(false);
    if (m_status) {
        m_status->setText(QStringLiteral("Checking the output path…"));
        m_status->setStyleSheet(QString());
    }
    m_validateTimer->start();
}

void RenderOptionsDialog::updateValidation() {
    if (!m_job || !m_startButton) return;
    const QVariantMap result = m_job->validateOptions(buildConfig());
    const bool valid = result.value(QStringLiteral("valid")).toBool();
    const QString message = result.value(QStringLiteral("message")).toString();
    m_startButton->setEnabled(valid && !m_job->running());
    if (m_status) {
        m_status->setText((valid ? QStringLiteral("Ready — ") : QStringLiteral("Not ready — ")) + message);
        const ThemeTokens& t = Theme::instance().current();
        m_status->setStyleSheet(QStringLiteral("color: %1;").arg((valid ? t.ok : t.danger).name()));
    }
}

void RenderOptionsDialog::framesFromTime() {
    if (!m_fps || !m_frames || !m_timeSeconds) return;
    const double fps = std::max(1, m_fps->value());
    const int frames = std::clamp(
        static_cast<int>(std::lround(m_timeSeconds->value() * fps)), 1, 1000000);
    const QSignalBlocker blockFrames(m_frames);
    m_frames->setValue(frames);
}

void RenderOptionsDialog::timeFromFrames() {
    if (!m_fps || !m_frames || !m_timeSeconds) return;
    const double fps = std::max(1, m_fps->value());
    const int frames = std::max(1, m_frames->value());
    const QSignalBlocker blockTime(m_timeSeconds);
    m_timeSeconds->setValue(static_cast<double>(frames) / fps);
}

void RenderOptionsDialog::updateSurfaceVisibility() {
    if (m_surfaceGroup && m_mode) m_surfaceGroup->setVisible(m_mode->currentIndex() == 1);
}

void RenderOptionsDialog::chooseOutputDirectory() {
    QString start = QDir::fromNativeSeparators(m_output->text().trimmed());
    QDir candidate(start);
    while (!candidate.exists() && !candidate.isRoot()) candidate.cdUp();
    const QString chosen = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose render output directory"), candidate.absolutePath());
    if (!chosen.isEmpty()) {
        m_output->setText(QDir::toNativeSeparators(chosen));
        scheduleValidation();
    }
}

void RenderOptionsDialog::startRender() {
    if (!m_job) return;
    const QVariantMap config = buildConfig();
    const QVariantMap result = m_job->validateOptions(config);
    if (!result.value(QStringLiteral("valid")).toBool()) {
        QMessageBox::warning(this, QStringLiteral("Render"), result.value(QStringLiteral("message")).toString());
        updateValidation();
        return;
    }
    if (!m_job->start(config)) {
        QMessageBox::warning(this, QStringLiteral("Render"),
                             QStringLiteral("The render process could not be started."));
        updateValidation();
        return;
    }
    Q_EMIT renderStarted();
    accept();
}

RenderProgressDialog::RenderProgressDialog(RenderJob* job, QWidget* parent)
    : QDialog(parent), m_job(job) {
    setWindowTitle(QStringLiteral("Rendering"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setModal(true);
    setFixedSize(600, 250);
    buildUi();
    setStyleSheet(QStringLiteral(
        "QLabel#RenderProgressTitle { font-size: 22px; font-weight: 600; }"
        "QLabel[role=\"caption\"] { font-size: 13px; }"
        "QPushButton { font-size: 13px; padding: 6px 14px; }"
        "QProgressBar { min-height: 14px; max-height: 14px; }"));

    if (m_job) {
        connect(m_job, &RenderJob::completed, this, &RenderProgressDialog::finishCompleted);
        connect(m_job, &RenderJob::failed, this, &RenderProgressDialog::finishFailed);
        connect(m_job, &RenderJob::cancelled, this, &RenderProgressDialog::finishCancelled);
    }
    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &RenderProgressDialog::updateProgress);
    m_timer->start();
    updateProgress();
}

void RenderProgressDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(10);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("Rendering"), this);
    title->setObjectName(QStringLiteral("RenderProgressTitle"));
    m_frame = Caption(QString());
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(m_frame);
    header->setAlignment(m_frame, Qt::AlignVCenter);
    root->addLayout(header);

    m_status = new QLabel(this);
    m_status->setProperty("role", "caption");
    root->addWidget(m_status);

    root->addWidget(Caption(QStringLiteral("Overall")));
    m_overall = new QProgressBar(this);
    m_overall->setRange(0, 1000);
    m_overall->setTextVisible(false);
    root->addWidget(m_overall);

    root->addWidget(Caption(QStringLiteral("Current stage")));
    m_stage = new QProgressBar(this);
    m_stage->setRange(0, 1000);
    m_stage->setTextVisible(false);
    root->addWidget(m_stage);

    root->addStretch(1);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(12);
    m_pauseButton = new QPushButton(QStringLiteral("Pause"), this);
    m_pauseButton->setProperty("variant", "ghost");
    m_pauseButton->setMinimumWidth(100);
    m_interruptButton = new QPushButton(QStringLiteral("Interrupt"), this);
    m_interruptButton->setMinimumWidth(100);
    m_times = Caption(QString());
    m_times->setAlignment(Qt::AlignCenter);
    buttons->addWidget(m_pauseButton);
    buttons->addStretch(1);
    buttons->addWidget(m_times);
    buttons->addStretch(1);
    buttons->addWidget(m_interruptButton);
    root->addLayout(buttons);

    connect(m_pauseButton, &QPushButton::clicked, this, &RenderProgressDialog::togglePause);
    connect(m_interruptButton, &QPushButton::clicked, this, &RenderProgressDialog::confirmInterrupt);
}

void RenderProgressDialog::updateProgress() {
    if (!m_job) return;
    m_overall->setValue(qRound(m_job->overallProgress() * 1000.0));
    m_stage->setValue(qRound(m_job->stageProgress() * 1000.0));
    m_status->setText(m_job->statusText());
    m_frame->setText(QStringLiteral("%1 / %2 frames").arg(m_job->frame()).arg(m_job->totalFrames()));
    m_times->setText(QStringLiteral("Elapsed %1   ·   Remaining %2")
                         .arg(FormatDuration(m_job->elapsedSeconds()),
                              FormatDuration(m_job->etaSeconds())));
    m_pauseButton->setText(m_job->paused() ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    m_pauseButton->setEnabled(m_job->running());
    m_interruptButton->setEnabled(m_job->running());
}

void RenderProgressDialog::togglePause() {
    if (m_job) m_job->togglePaused();
}

void RenderProgressDialog::confirmInterrupt() {
    if (!m_job || !m_job->running()) return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Interrupt render"),
        QStringLiteral("Interrupt the running render?\n\nFiles already written stay in the output directory."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    m_interruptButton->setEnabled(false);
    m_pauseButton->setEnabled(false);
    m_status->setText(QStringLiteral("Cancelling…"));
    m_job->interrupt();
}

void RenderProgressDialog::finishCompleted() {
    if (m_finished) return;
    m_finished = true;
    if (m_timer) m_timer->stop();
    hide();
    QMessageBox::information(parentWidget(), QStringLiteral("Render"),
                             QStringLiteral("The rendering has ended"));
    accept();
}

void RenderProgressDialog::finishFailed(const QString& message) {
    if (m_finished) return;
    m_finished = true;
    if (m_timer) m_timer->stop();
    hide();
    QMessageBox::warning(parentWidget(), QStringLiteral("Render failed"), message);
    reject();
}

void RenderProgressDialog::finishCancelled() {
    if (m_finished) return;
    m_finished = true;
    if (m_timer) m_timer->stop();
    accept();
}

bool RenderProgressDialog::confirmClose() {
    if (m_finished || !m_job || !m_job->running()) return true;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Interrupt render"),
        QStringLiteral("Close this window and interrupt the render?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return false;
    m_finished = true;
    m_job->interrupt();
    return true;
}

void RenderProgressDialog::closeEvent(QCloseEvent* event) {
    if (confirmClose())
        event->accept();
    else
        event->ignore();
}

void RenderProgressDialog::reject() {
    if (confirmClose()) QDialog::reject();
}

}  // namespace ui
