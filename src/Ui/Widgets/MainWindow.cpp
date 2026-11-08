#include "Ui/Widgets/MainWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QThread>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "Render/GlView.hpp"
#include "Render/RenderJob.hpp"
#include "Session/SimulationSession.hpp"
#include "Ui/Icons.hpp"
#include "Ui/Theme.hpp"
#include "Ui/Widgets/Controls.hpp"
#include "Ui/Widgets/Panels.hpp"
#include "Ui/Widgets/RenderDialogs.hpp"
#include "Ui/Widgets/Viewport.hpp"

namespace ui {
namespace {

QString FormatBytes(qlonglong bytes) {
    if (bytes <= 0) return QStringLiteral("—");
    if (bytes >= 1024ll * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) +
               QStringLiteral(" GB");
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 0) + QStringLiteral(" MB");
}

// "33412" -> "33 412"
QString GroupDigits(qint64 value) {
    const QString raw = QString::number(value);
    QString out;
    int count = 0;
    for (int i = raw.size() - 1; i >= 0; --i) {
        out.prepend(raw.at(i));
        if (++count % 3 == 0 && i > 0 && raw.at(i - 1).isDigit()) out.prepend(QLatin1Char(' '));
    }
    return out;
}

QIcon ThemeSwatchIcon(const ThemeTokens& t) {
    constexpr int kSize = 18;
    QPixmap pm(kSize, kSize);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal radius = t.square ? 0.0 : 4.0;
    p.setPen(QPen(t.border, 1));
    p.setBrush(t.bg);
    p.drawRoundedRect(QRectF(0.5, 0.5, kSize - 1, kSize - 1), radius, radius);
    p.setPen(Qt::NoPen);
    p.setBrush(t.card);
    p.drawRoundedRect(QRectF(4, 4, 10, 7), qMax(0.0, radius - 2), qMax(0.0, radius - 2));
    p.setBrush(t.accent);
    p.drawRoundedRect(QRectF(4, 13, 7, 2), t.square ? 0.0 : 1.0, t.square ? 0.0 : 1.0);
    return QIcon(pm);
}

QIcon GlyphIcon(IconKind kind, const QColor& ink, int size = 16) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.translate(size / 2.0, size / 2.0);
    PaintIcon(p, kind, ink, size);
    return QIcon(pm);
}

}  // namespace

MainWindow::MainWindow(SimulationSession* session, QWidget* parent)
    : QWidget(parent), m_session(session) {
    setObjectName(QStringLiteral("Root"));
    setWindowTitle(QStringLiteral("SPHFluidSim — Implicit Incompressible SPH"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumSize(1120, 700);
    setMouseTracking(true);

    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    resize(std::min(1580, avail.width() - 80), std::min(960, avail.height() - 90));

    m_renderJob = new RenderJob(this);
    buildUi();
    wire();
    applyTheme();
    syncParams();
    syncStatus();
    updateTransport();
    layoutTransport();
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------
void MainWindow::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addWidget(buildTitleBar());
    outer->addWidget(buildMenuBar());

    auto* bodyHost = new QWidget(this);
    auto* body = new QHBoxLayout(bodyHost);
    body->setContentsMargins(10, 10, 10, 10);
    body->setSpacing(10);

    m_leftPanel = new SidePanel(bodyHost);
    m_leftPanel->setFixedWidth(272);
    buildLeftPanel();

    m_viewport = new Viewport(m_session, bodyHost);

    m_rightPanel = new SidePanel(bodyHost);
    m_rightPanel->setFixedWidth(300);
    buildRightPanel();

    body->addWidget(m_leftPanel);
    body->addWidget(m_viewport, 1);
    body->addWidget(m_rightPanel);
    outer->addWidget(bodyHost, 1);

    outer->addWidget(buildStatusBar());
}

QWidget* MainWindow::buildTitleBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("TitleBar"));
    bar->setFixedHeight(52);
    bar->installEventFilter(this);
    m_titleBar = bar;

    auto* grid = new QGridLayout(bar);
    grid->setContentsMargins(14, 0, 14, 0);
    grid->setHorizontalSpacing(16);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(2, 1);

    // --- brand ---
    auto* brand = new QWidget(bar);
    auto* brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(0, 0, 0, 0);
    brandLayout->setSpacing(10);

    auto* mark = new QLabel(QStringLiteral("SPH"), brand);
    mark->setObjectName(QStringLiteral("BrandMark"));
    mark->setFixedSize(26, 26);
    mark->setAlignment(Qt::AlignCenter);

    auto* names = new QWidget(brand);
    auto* nameLayout = new QVBoxLayout(names);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->setSpacing(0);
    auto* name = new QLabel(QStringLiteral("SPHFluidSim"), names);
    name->setProperty("role", "brandName");
    auto* sub = new QLabel(QStringLiteral("Implicit Incompressible SPH"), names);
    sub->setProperty("role", "brandSub");
    nameLayout->addWidget(name);
    nameLayout->addWidget(sub);

    brandLayout->addWidget(mark);
    brandLayout->addWidget(names);
    grid->addWidget(brand, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

    // --- transport ---------------------------------------------------------
    // These controls are positioned manually in layoutTransport(): the pause
    // button must sit at the exact window centre, independent of the unequal
    // widths of the brand and right-hand clusters.
    m_resetButton = new IconButton(IconKind::Reset, bar);
    m_resetButton->setFixedSize(28, 28);
    m_resetButton->setToolTip(QStringLiteral("Reset scene (R)"));
    m_playButton = new IconButton(IconKind::Play, bar);
    m_playButton->setFixedSize(32, 32);
    m_playButton->setCircular(true);
    m_playButton->setPrimary(true);
    m_playButton->setToolTip(QStringLiteral("Pause (Space)"));
    m_screenshotButton = new IconButton(IconKind::Camera, bar);
    m_screenshotButton->setFixedSize(28, 28);
    m_screenshotButton->setIconSize(18);
    m_screenshotButton->setToolTip(QStringLiteral("Save screenshot (Ctrl+S)"));

    m_clock = new QLabel(QStringLiteral("t 0.00 s"), bar);
    m_clock->setObjectName(QStringLiteral("Clock"));
    m_clock->setFixedSize(84, 22);
    m_clock->setAlignment(Qt::AlignCenter);

    // --- right side ---
    auto* right = new QWidget(bar);
    auto* rightLayout = new QHBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);

    m_viewModeSeg = new SegmentedControl(right);
    m_viewModeSeg->setOptions({QStringLiteral("Particles"), QStringLiteral("Surface"),
                               QStringLiteral("Both")});
    m_viewModeSeg->setCurrentIndex(0);

    m_fullscreenButton = new IconButton(IconKind::Fullscreen, right);
    m_fullscreenButton->setToolTip(QStringLiteral("Fullscreen (F11)"));

    auto makeWindowButton = [right](IconKind kind, const QString& tip) {
        auto* button = new IconButton(kind, right);
        button->setFixedSize(30, 26);
        button->setIconSize(12);
        button->setToolTip(tip);
        return button;
    };
    m_minimizeButton = makeWindowButton(IconKind::WindowMinimize,
                                        QStringLiteral("Minimize"));
    m_maximizeButton = makeWindowButton(IconKind::WindowMaximize,
                                        QStringLiteral("Maximize / restore"));
    m_closeButton = makeWindowButton(IconKind::WindowClose, QStringLiteral("Close"));
    m_closeButton->setDanger(true);

    rightLayout->addWidget(m_viewModeSeg);
    rightLayout->addSpacing(4);
    rightLayout->addWidget(m_fullscreenButton);
    rightLayout->addSpacing(4);
    rightLayout->addWidget(m_minimizeButton);
    rightLayout->addWidget(m_maximizeButton);
    rightLayout->addWidget(m_closeButton);
    grid->addWidget(right, 0, 2, Qt::AlignRight | Qt::AlignVCenter);

    return bar;
}

void MainWindow::layoutTransport() {
    if (!m_titleBar || !m_playButton || !m_resetButton || !m_screenshotButton || !m_clock) return;

    const int h = m_titleBar->height();
    const int cx = m_titleBar->width() / 2;
    const int playTop = (h - 32) / 2;
    m_playButton->setGeometry(cx - 16, playTop, 32, 32);

    const int smallTop = (h - 28) / 2;
    m_resetButton->setGeometry(m_playButton->x() - 6 - 28, smallTop, 28, 28);
    m_screenshotButton->setGeometry(m_playButton->x() + 32 + 6, smallTop, 28, 28);
    m_clock->setGeometry(m_screenshotButton->x() + 28 + 8, (h - 22) / 2, 84, 22);

    m_resetButton->raise();
    m_playButton->raise();
    m_screenshotButton->raise();
    m_clock->raise();
}

QWidget* MainWindow::buildMenuBar() {
    auto* bar = new QMenuBar(this);
    bar->setObjectName(QStringLiteral("AppMenuBar"));
    bar->setNativeMenuBar(false);
    bar->setFixedHeight(30);

    QMenu* file = bar->addMenu(QStringLiteral("File"));
    QAction* importObj = file->addAction(QStringLiteral("Import 3D Model..."));
    connect(importObj, &QAction::triggered, this,
            [this] { m_session->importModel(); });
    QAction* importParameters = file->addAction(QStringLiteral("Import Parameter (Json)..."));
    importParameters->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(importParameters, &QAction::triggered, this,
            [this] { m_session->importParameters(); });
    QAction* exportParameters = file->addAction(QStringLiteral("Export Parameter (Json)..."));
    exportParameters->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(exportParameters, &QAction::triggered, this,
            [this] { m_session->exportParameters(); });
    file->addSeparator();
    QAction* restart = file->addAction(QStringLiteral("Restart scene"));
    connect(restart, &QAction::triggered, this,
            [this] { m_session->resetSimulation(); });
    QAction* snapshot = file->addAction(QStringLiteral("Save snapshot"));
    connect(snapshot, &QAction::triggered, this, [this] { saveScreenshot(); });
    file->addSeparator();
    QAction* exit = file->addAction(QStringLiteral("Exit"));
    exit->setShortcut(QKeySequence::Quit);
    connect(exit, &QAction::triggered, this, &QWidget::close);

    QAction* render = bar->addAction(QStringLiteral("Render"));
    render->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    connect(render, &QAction::triggered, this, &MainWindow::openRenderDialog);

    m_appearanceMenu = bar->addMenu(QStringLiteral("Appearance"));
    for (int i = 0; i < Theme::count(); ++i) {
        const ThemeTokens& tokens = Theme::tokens(i);
        QAction* action =
            m_appearanceMenu->addAction(ThemeSwatchIcon(tokens), tokens.name);
        action->setCheckable(true);
        action->setData(i);
        connect(action, &QAction::triggered, this,
                [i] { Theme::instance().setIndex(i); });
    }
    m_appearanceMenu->addSeparator();
    QAction* nextTheme = m_appearanceMenu->addAction(QStringLiteral("Next theme"));
    connect(nextTheme, &QAction::triggered, this,
            [] { Theme::instance().cycle(); });

    QMenu* help = bar->addMenu(QStringLiteral("Help"));
    QAction* shortcuts = help->addAction(QStringLiteral("Keyboard shortcuts"));
    connect(shortcuts, &QAction::triggered, this, [this] {
        QMessageBox::information(
            this, QStringLiteral("Keyboard shortcuts"),
            QStringLiteral("Space   Pause / resume\n"
                           "S       Single step (Orbit) / move back (Fly)\n"
                           "R       Restart the scene\n"
                           "Ctrl+T  Cycle appearance\n"
                           "Ctrl+S  Save screenshot\n"
                           "Ctrl+Shift+O  Import parameters (JSON)\n"
                           "Ctrl+Shift+S  Export parameters (JSON)\n"
                           "Ctrl+Shift+R  Open render settings\n"
                           "F11     Toggle fullscreen\n"
                           "1/2/3   Particles / surface / both\n\n"
                           "Camera has two modes: Orbit (left-drag orbits) and Fly "
                           "(left-drag looks, WASD moves, Q/E vertical). "
                           "Middle/right-drag pans, the wheel changes the field of view."));
    });
    QAction* about = help->addAction(QStringLiteral("About SPHFluidSim"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("About SPHFluidSim"));
        box.setIcon(QMessageBox::NoIcon);
        box.setTextFormat(Qt::PlainText);
        box.setText(QStringLiteral(
            "SPHFluidSim\n"
            "Real-time implicit incompressible SPH fluid simulation.\n\n"
            "IISPH pressure solve (Ihmsen et al., TVCG 2013) with a DFSPH "
            "alternative (Bender & Koschier, 2017), Akinci-style surface "
            "reconstruction, Qt Widgets / Qt Quick frontends and an OpenGL 3.3 "
            "renderer.\n\n"
            "https://github.com/EMEEEEMMMM/SPHFluidSim"));
        QPushButton* open = box.addButton(QStringLiteral("Open on GitHub"),
                                          QMessageBox::ActionRole);
        box.addButton(QStringLiteral("Close"), QMessageBox::RejectRole);
        connect(open, &QAbstractButton::clicked, &box, [] {
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://github.com/EMEEEEMMMM/SPHFluidSim")));
        });
        box.exec();
    });

    return bar;
}

void MainWindow::buildLeftPanel() {
    // --- scene ---
    Card* scene = m_leftPanel->addCard(QStringLiteral("Scene"));
    scene->setBadge(QStringLiteral("open tank"));

    m_sceneSeg = new SegmentedControl(scene);
    m_sceneSeg->setOptions({QStringLiteral("Dam break"), QStringLiteral("Inflow")});
    scene->addWidget(m_sceneSeg);

    auto* resRow = new QWidget(scene);
    auto* resLayout = new QHBoxLayout(resRow);
    resLayout->setContentsMargins(0, 0, 0, 0);
    resLayout->setSpacing(8);
    auto* resLabel = new QLabel(QStringLiteral("Particles"), resRow);
    resLabel->setProperty("role", "caption");
    m_particles = new QSpinBox(resRow);
    m_particles->setRange(512, std::numeric_limits<int>::max());
    m_particles->setSingleStep(1024);
    m_particles->setGroupSeparatorShown(true);
    // Without keyboard tracking the scene is rebuilt once, when the number is
    // committed (Enter / focus out / arrow), not on every typed digit.
    m_particles->setKeyboardTracking(false);
    m_particles->setValue(32768);
    m_particles->setToolTip(QStringLiteral("Exact number of fluid particles; the spacing follows from it"));
    resLayout->addWidget(resLabel);
    resLayout->addStretch(1);
    resLayout->addWidget(m_particles);
    scene->addWidget(resRow);

    m_spacingHint = scene->addValueRow(QStringLiteral("Spacing"), QStringLiteral("—"));

    m_particleWarning = new QLabel(scene);
    m_particleWarning->setWordWrap(true);
    m_particleWarning->setVisible(false);
    m_particleWarning->setStyleSheet(
        QStringLiteral("color: %1; font-size: 10px;")
            .arg(Theme::instance().current().danger.name()));
    scene->addWidget(m_particleWarning);

    m_particleCount = scene->addValueRow(QStringLiteral("Active"), QStringLiteral("—"));

    m_dropButton = new QPushButton(QStringLiteral("Spawn water drop"), scene);
    m_dropButton->setIcon(GlyphIcon(IconKind::Droplet, Theme::instance().current().fg, 13));
    m_dropButton->setIconSize(QSize(13, 13));
    m_dropButton->setCursor(Qt::PointingHandCursor);
    scene->addWidget(m_dropButton);

    // --- playback ---
    Card* playback = m_leftPanel->addCard(QStringLiteral("Playback"));
    m_playbackCard = playback;
    playback->setBadge(QStringLiteral("paused"));

    m_speed = new SliderRow(QStringLiteral("Speed"), playback);
    m_speed->setRange(1.0, 240.0);
    m_speed->setDecimals(0);
    m_speed->setSuffix(QStringLiteral(" steps/s"));
    playback->addWidget(m_speed);

    m_actualRate = playback->addValueRow(QStringLiteral("Measured step rate"),
                                         QStringLiteral("—"));
    m_stepCost = playback->addValueRow(QStringLiteral("Step cost"), QStringLiteral("—"));

    // --- metrics ---
    Card* metrics = m_leftPanel->addCard(QStringLiteral("Metrics"));
    metrics->setBadge(QStringLiteral("live"));
    metrics->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    m_metrics = new MetricGrid(metrics);
    m_metrics->setMetrics({QStringLiteral("Sim time"), QStringLiteral("Density err"),
                           QStringLiteral("Pressure iters"), QStringLiteral("Triangles")});
    metrics->addWidget(m_metrics);

    m_traceValue = metrics->addValueRow(QStringLiteral("Mean density"),
                                        QStringLiteral("—"));
    m_spark = new Sparkline(metrics);
    m_spark->setMinimumHeight(120);
    m_spark->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    metrics->addWidget(m_spark);

    m_traceLegend = new QLabel(metrics);
    m_traceLegend->setProperty("role", "legend");
    m_traceLegend->setTextFormat(Qt::RichText);
    metrics->addWidget(m_traceLegend);
    refreshTraceLegend();
}

void MainWindow::refreshTraceLegend() {
    if (!m_traceLegend) return;
    const ThemeTokens& t = Theme::instance().current();
    m_traceLegend->setText(
        QStringLiteral("<span style=\"color:%1\">err</span>&nbsp;&nbsp;"
                       "<span style=\"color:%2\">●</span>&nbsp;%3&nbsp;&nbsp;&nbsp;"
                       "<span style=\"color:%4\">●</span>&nbsp;%5&nbsp;&nbsp;&nbsp;"
                       "<span style=\"color:%6\">●</span>&nbsp;%7")
            .arg(t.fg3.name(), t.ok.name(), QStringLiteral("≤0.1 %"),
                 t.warn.name(), QStringLiteral("0.1-1 %"),
                 t.danger.name(), QStringLiteral("≥1 %")));
}

void MainWindow::buildRightPanel() {
    // --- solver ---
    Card* solver = m_rightPanel->addCard(QStringLiteral("Solver"));
    solver->setBadge(QStringLiteral("IISPH"));

    m_solverSeg = new SegmentedControl(solver);
    m_solverSeg->setOptions({QStringLiteral("IISPH"), QStringLiteral("DFSPH")});
    solver->addWidget(m_solverSeg);

    auto* advancedHeader = new QWidget(solver);
    advancedHeader->setCursor(Qt::PointingHandCursor);
    advancedHeader->setProperty("advHeader", true);
    advancedHeader->installEventFilter(this);
    auto* advLayout = new QHBoxLayout(advancedHeader);
    advLayout->setContentsMargins(0, 2, 0, 0);
    advLayout->setSpacing(4);
    m_advancedArrow = new IconButton(IconKind::Chevron, advancedHeader);
    m_advancedArrow->setFixedSize(18, 18);
    m_advancedArrow->setIconSize(11);
    auto* advLabel = new QLabel(QStringLiteral("Advanced"), advancedHeader);
    advLabel->setProperty("role", "caption");
    advLayout->addWidget(m_advancedArrow);
    advLayout->addWidget(advLabel);
    advLayout->addStretch(1);
    solver->addWidget(advancedHeader);

    m_advancedBody = new QWidget(solver);
    auto* advBodyLayout = new QVBoxLayout(m_advancedBody);
    advBodyLayout->setContentsMargins(0, 0, 0, 0);
    advBodyLayout->setSpacing(7);

    m_omega = new SliderRow(QStringLiteral("omega"), m_advancedBody);
    m_omega->setRange(0.1, 0.9);
    m_eta = new SliderRow(QStringLiteral("eta"), m_advancedBody);
    m_eta->setRange(0.0001, 0.01);
    m_eta->setDecimals(4);
    m_etaV = new SliderRow(QStringLiteral("eta_v"), m_advancedBody);
    m_etaV->setRange(0.0001, 0.01);
    m_etaV->setDecimals(4);
    m_maxIter = new SliderRow(QStringLiteral("Max iterations"), m_advancedBody);
    m_maxIter->setRange(2, 100);
    m_maxIter->setDecimals(0);
    m_dt = new SliderRow(QStringLiteral("dt"), m_advancedBody);
    m_dt->setRange(0.0005, 0.01);
    m_dt->setDecimals(4);
    m_dt->setSuffix(QStringLiteral(" s"));

    auto* cflRow = new QWidget(m_advancedBody);
    auto* cflLayout = new QHBoxLayout(cflRow);
    cflLayout->setContentsMargins(0, 0, 0, 0);
    auto* cflLabel = new QLabel(QStringLiteral("CFL step limiter"), cflRow);
    cflLabel->setProperty("role", "caption");
    m_cfl = new ToggleSwitch(cflRow);
    cflLayout->addWidget(cflLabel);
    cflLayout->addStretch(1);
    cflLayout->addWidget(m_cfl);

    advBodyLayout->addWidget(m_omega);
    advBodyLayout->addWidget(m_eta);
    advBodyLayout->addWidget(m_etaV);
    advBodyLayout->addWidget(m_maxIter);
    advBodyLayout->addWidget(m_dt);
    advBodyLayout->addWidget(cflRow);
    m_advancedBody->setVisible(false);
    solver->addWidget(m_advancedBody);

    // --- forces ---
    Card* forces = m_rightPanel->addCard(QStringLiteral("Forces"));
    forces->setBadge(QStringLiteral("Sec. 5"));
    m_viscosity = new SliderRow(QStringLiteral("XSPH viscosity"), forces);
    m_viscosity->setRange(0.0, 1.0);
    m_tension = new SliderRow(QStringLiteral("Surface tension"), forces);
    m_tension->setRange(0.0, 0.5);
    m_adhesion = new SliderRow(QStringLiteral("Wall adhesion"), forces);
    m_adhesion->setRange(0.0, 0.1);
    forces->addWidget(m_viscosity);
    forces->addWidget(m_tension);
    forces->addWidget(m_adhesion);

    // --- surface ---
    Card* surface = m_rightPanel->addCard(QStringLiteral("Surface"));
    surface->setBadge(QStringLiteral("Akinci 2012"));
    m_threshold = new SliderRow(QStringLiteral("Threshold"), surface);
    m_threshold->setRange(0.3, 0.95);
    m_threshold->setDecimals(2);
    m_gridScale = new SliderRow(QStringLiteral("Grid scale"), surface);
    m_gridScale->setRange(0.5, 2.0);
    m_gridScale->setDecimals(1);
    m_gridScale->setSuffix(QStringLiteral(" × r"));
    m_reconInterval = new SliderRow(QStringLiteral("Rebuild every"), surface);
    m_reconInterval->setRange(1, 20);
    m_reconInterval->setDecimals(0);
    m_reconInterval->setSuffix(QStringLiteral(" frames"));
    surface->addWidget(m_threshold);
    surface->addWidget(m_gridScale);
    surface->addWidget(m_reconInterval);

    // --- camera ---
    Card* camera = m_rightPanel->addCard(QStringLiteral("Camera"));
    camera->setBadge(QStringLiteral("orbit"));
    m_cameraSeg = new SegmentedControl(camera);
    m_cameraSeg->setOptions({QStringLiteral("Orbit"), QStringLiteral("Fly")});
    camera->addWidget(m_cameraSeg);

    m_fov = new SliderRow(QStringLiteral("Field of view"), camera);
    m_fov->setRange(20.0, 75.0);
    m_fov->setDecimals(0);
    m_fov->setSuffix(QStringLiteral("°"));
    camera->addWidget(m_fov);

    auto* resetRow = new QPushButton(QStringLiteral("Reset view"), camera);
    resetRow->setProperty("variant", "ghost");
    resetRow->setCursor(Qt::PointingHandCursor);
    camera->addWidget(resetRow);
    connect(resetRow, &QPushButton::clicked, this, [this] {
        if (m_viewport) m_viewport->resetCamera();
    });

    // --- overlay ---
    Card* overlay = m_rightPanel->addCard(QStringLiteral("Overlay"));
    auto* perfRow = new QWidget(overlay);
    auto* perfLayout = new QHBoxLayout(perfRow);
    perfLayout->setContentsMargins(0, 0, 0, 0);
    auto* perfLabel = new QLabel(QStringLiteral("Performance HUD"), perfRow);
    perfLabel->setProperty("role", "caption");
    m_perfHud = new ToggleSwitch(perfRow);
    m_perfHud->setChecked(true);
    perfLayout->addWidget(perfLabel);
    perfLayout->addStretch(1);
    perfLayout->addWidget(m_perfHud);
    overlay->addWidget(perfRow);

    auto* sceneRow = new QWidget(overlay);
    auto* sceneLayout = new QHBoxLayout(sceneRow);
    sceneLayout->setContentsMargins(0, 0, 0, 0);
    auto* sceneLabel = new QLabel(QStringLiteral("Scene label"), sceneRow);
    sceneLabel->setProperty("role", "caption");
    m_sceneHud = new ToggleSwitch(sceneRow);
    m_sceneHud->setChecked(true);
    sceneLayout->addWidget(sceneLabel);
    sceneLayout->addStretch(1);
    sceneLayout->addWidget(m_sceneHud);
    overlay->addWidget(sceneRow);

    m_rightPanel->addStretch();
}

QWidget* MainWindow::buildStatusBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("StatusBar"));
    bar->setFixedHeight(26);

    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(14, 0, 14, 0);
    layout->setSpacing(12);

    m_statusLeft = new QLabel(bar);
    m_statusLeft->setProperty("role", "status");
    m_statusRight = new QLabel(QStringLiteral("—"), bar);
    m_statusRight->setProperty("role", "status");

    layout->addWidget(m_statusLeft);
    layout->addStretch(1);
    layout->addWidget(m_statusRight);
    return bar;
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------
void MainWindow::wire() {
    connect(m_session, &SimulationSession::paramsChanged, this, &MainWindow::syncParams);
    connect(m_session, &SimulationSession::statusChanged, this, &MainWindow::syncStatus);
    connect(m_session, &SimulationSession::traceChanged, this, &MainWindow::syncTrace);
    connect(m_session, &SimulationSession::runningChanged, this, &MainWindow::updateTransport);
    connect(m_session, &SimulationSession::message, this, &MainWindow::toast);
    connect(m_viewport, &Viewport::fpsChanged, this, &MainWindow::updateHud);
    connect(&Theme::instance(), &Theme::changed, this, &MainWindow::applyTheme);

    connect(m_playButton, &QAbstractButton::clicked, this,
            [this] { m_session->toggleRunning(); });
    connect(m_resetButton, &QAbstractButton::clicked, this,
            [this] { m_session->resetSimulation(); });
    connect(m_screenshotButton, &QAbstractButton::clicked, this,
            [this] { saveScreenshot(); });
    connect(m_fullscreenButton, &QAbstractButton::clicked, this, [this] {
        if (isFullScreen()) showNormal();
        else showFullScreen();
    });
    connect(m_minimizeButton, &QAbstractButton::clicked, this, &QWidget::showMinimized);
    connect(m_maximizeButton, &QAbstractButton::clicked, this, [this] {
        if (isMaximized()) showNormal();
        else showMaximized();
    });
    connect(m_closeButton, &QAbstractButton::clicked, this, &QWidget::close);

    connect(m_viewModeSeg, &SegmentedControl::activated, this, [this](int index) {
        setParam(QStringLiteral("viewMode"), index);
        m_viewport->setViewMode(index);
    });

    connect(m_sceneSeg, &SegmentedControl::activated, this, &MainWindow::changeScene);
    connect(m_particles, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &MainWindow::changeParticleCount);    connect(m_dropButton, &QPushButton::clicked, this, [this] { m_session->spawnDrop(); });

    connect(m_speed, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("speed"), static_cast<int>(std::lround(v))); });

    connect(m_solverSeg, &SegmentedControl::activated, this, &MainWindow::changeSolver);

    connect(m_advancedArrow, &QAbstractButton::clicked, this, [this] {
        const bool show = !m_advancedBody->isVisible();
        m_advancedBody->setVisible(show);
        m_advancedArrow->setRotation(show ? 90.0 : 0.0);
    });

    connect(m_omega, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("omega"), v); });
    connect(m_eta, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("eta"), v); });
    connect(m_etaV, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("etaV"), v); });
    connect(m_maxIter, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("maxIter"), static_cast<int>(std::lround(v))); });
    connect(m_dt, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("dt"), v); });
    connect(m_cfl, &ToggleSwitch::toggled, this,
            [this](bool on) { setParam(QStringLiteral("cfl"), on); });

    connect(m_viscosity, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("viscosity"), v); });
    connect(m_tension, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("surfaceTension"), v); });
    connect(m_adhesion, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("wallAdhesion"), v); });

    connect(m_threshold, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("surfaceThreshold"), v); });
    connect(m_gridScale, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("gridScale"), v); });
    connect(m_reconInterval, &SliderRow::valueChanged, this,
            [this](double v) { setParam(QStringLiteral("reconInterval"), static_cast<int>(std::lround(v))); });

    connect(m_cameraSeg, &SegmentedControl::activated, this, [this](int index) {
        if (!m_viewport) return;
        m_viewport->setOrbitOnly(index == 0);
        m_viewport->setFocus(Qt::MouseFocusReason);
    });
    connect(m_fov, &SliderRow::valueChanged, this, [this](double v) {
        if (m_viewport && m_viewport->view()) {
            m_viewport->view()->cameraState().fovDeg = static_cast<float>(v);
        }
    });
    connect(m_viewport, &Viewport::cameraReset, this, [this] {
        if (m_viewport && m_viewport->view()) {
            m_fov->setValue(m_viewport->view()->cameraState().fovDeg);
        }
    });
    connect(m_viewport, &Viewport::fieldOfViewChanged, this, [this](double degrees) {
        if (m_fov) m_fov->setValue(degrees);
    });

    connect(m_perfHud, &ToggleSwitch::toggled, this, [this](bool) { updateHud(); });
    connect(m_sceneHud, &ToggleSwitch::toggled, this, [this](bool) { updateHud(); });
}

// ---------------------------------------------------------------------------
// State sync
// ---------------------------------------------------------------------------
void MainWindow::syncParams() {
    if (m_syncing) return;
    m_syncing = true;

    const QVariantMap p = m_session->params();
    const int scene = p.value(QStringLiteral("scene")).toInt();
    const int solver = p.value(QStringLiteral("solver")).toInt();
    const int viewMode = p.value(QStringLiteral("viewMode")).toInt();

    if (m_sceneSeg->currentIndex() != scene) m_sceneSeg->setCurrentIndex(scene);
    if (m_solverSeg->currentIndex() != solver) m_solverSeg->setCurrentIndex(solver);
    if (m_viewModeSeg->currentIndex() != viewMode) {
        m_viewModeSeg->setCurrentIndex(viewMode);
        m_viewport->setViewMode(viewMode);
    }

    {
        const QSignalBlocker block(m_particles);
        const int count = p.value(QStringLiteral("particleCount")).toInt();
        if (m_particles->value() != count) m_particles->setValue(count);
        if (m_particleWarning && m_particles->value() == count) {
            m_particleWarning->clear();
            m_particleWarning->setVisible(false);
        }
    }

    m_dropButton->setVisible(scene == 0);
    m_dropButton->setEnabled(scene == 0);

    m_speed->setValue(p.value(QStringLiteral("speed")).toDouble());

    m_omega->setValue(p.value(QStringLiteral("omega")).toDouble());
    m_eta->setValue(p.value(QStringLiteral("eta")).toDouble());
    m_etaV->setValue(p.value(QStringLiteral("etaV")).toDouble());
    m_maxIter->setValue(p.value(QStringLiteral("maxIter")).toDouble());
    m_dt->setValue(p.value(QStringLiteral("dt")).toDouble());
    m_cfl->setChecked(p.value(QStringLiteral("cfl")).toBool());
    m_eta->setVisible(solver == 0);
    m_etaV->setVisible(solver == 1);

    m_viscosity->setValue(p.value(QStringLiteral("viscosity")).toDouble());
    m_tension->setValue(p.value(QStringLiteral("surfaceTension")).toDouble());
    m_adhesion->setValue(p.value(QStringLiteral("wallAdhesion")).toDouble());

    m_threshold->setValue(p.value(QStringLiteral("surfaceThreshold")).toDouble());
    m_gridScale->setValue(p.value(QStringLiteral("gridScale")).toDouble());
    m_reconInterval->setValue(p.value(QStringLiteral("reconInterval")).toDouble());

    if (m_viewport && m_viewport->view()) {
        m_fov->setValue(m_viewport->view()->cameraState().fovDeg);
    }

    m_syncing = false;
}

void MainWindow::syncStatus() {
    const QVariantMap s = m_session->status();
    const double simTime = s.value(QStringLiteral("simTime")).toDouble();
    const int fluid = s.value(QStringLiteral("fluidCount")).toInt();
    const int boundary = s.value(QStringLiteral("boundaryCount")).toInt();
    const double density = s.value(QStringLiteral("densityError")).toDouble();
    const double meanDensity = s.value(QStringLiteral("meanDensity"), 1000.0).toDouble();
    const double rho0 = s.value(QStringLiteral("rho0"), 1000.0).toDouble();
    const int iterations = s.value(QStringLiteral("pressureIterations")).toInt();
    const double ms = s.value(QStringLiteral("msPerStep")).toDouble();
    const int scene = s.value(QStringLiteral("scene")).toInt();
    const double spacing = s.value(QStringLiteral("spacing")).toDouble();

    m_clock->setText(QStringLiteral("t %1 s").arg(simTime, 0, 'f', 2));
    m_particleCount->setText(GroupDigits(fluid));
    if (m_spacingHint) {
        const auto mem = m_session->status();
        const qlonglong bytesPerParticle =
            mem.value(QStringLiteral("bytesPerParticle")).toLongLong();
        const qlonglong freeBytes = mem.value(QStringLiteral("vramFreeBytes")).toLongLong();
        QString hint = QStringLiteral("%1 m").arg(spacing, 0, 'f', 4);
        if (bytesPerParticle > 0 && freeBytes > 0) {
            hint += QStringLiteral(" · %1 / %2 free")
                        .arg(FormatBytes(bytesPerParticle *
                                         m_session->params().value(QStringLiteral("particleCount")).toLongLong()),
                             FormatBytes(freeBytes));
        }
        m_spacingHint->setText(hint);
    }

    m_metrics->setValue(0, QString::number(simTime, 'f', 2) + QStringLiteral(" s"));
    m_metrics->setValue(1, QString::number(std::abs(density) * 100.0, 'f', 3) + QStringLiteral(" %"));
    const ThemeTokens& tk = Theme::instance().current();
    m_metrics->setValueColor(
        1, Sparkline::BandColor(tk.ok, tk.warn, tk.danger, density));
    m_metrics->setValue(2, QString::number(iterations));
    m_metrics->setValue(3, GroupDigits(m_viewport->view() ? m_viewport->view()->TriangleCount() : 0));
    m_traceValue->setText(QString::number(meanDensity, 'f', 2) + QStringLiteral(" kg/m³"));
    if (m_spark) m_spark->setReferenceDensity(rho0);

    m_stepCost->setText(QString::number(ms, 'f', 1) + QStringLiteral(" ms"));
    if (ms > 0.01) {
        m_actualRate->setText(QString::number(1000.0 / ms, 'f', 0) + QStringLiteral(" steps/s"));
    }

    m_statusRight->setText(QStringLiteral("%1 fluid · %2 boundary · r=%3 m")
                               .arg(GroupDigits(fluid), GroupDigits(boundary),
                                    QString::number(spacing, 'f', 4)));

    if (m_statusMessage.isEmpty()) {
        m_statusLeft->clear();
        m_statusLeft->setStyleSheet(QString());
    }

    updateHud();
}

void MainWindow::syncTrace() {
    const QVariantList trace = m_session->densityTrace();
    const double rho0 = m_session->status().value(QStringLiteral("rho0"), 1000.0).toDouble();
    QVector<DensitySample> samples;
    samples.reserve(trace.size());
    for (const QVariant& item : trace) {
        const QVariantMap point = item.toMap();
        DensitySample sample;
        sample.simTime = point.value(QStringLiteral("t")).toDouble();
        const double error = point.value(QStringLiteral("e")).toDouble();
        sample.density = point.value(QStringLiteral("density"),
                                     rho0 * (1.0 + error)).toDouble();
        sample.error = error;
        sample.step = point.value(QStringLiteral("step")).toULongLong();
        if (sample.density > rho0 * 0.5 && sample.density < rho0 * 1.5)
            samples.append(sample);
    }
    m_spark->setSamples(samples);
}

void MainWindow::updateTransport() {
    const bool running = m_session->running();
    m_playButton->setKind(running ? IconKind::Pause : IconKind::Play);
    m_playButton->setToolTip(running ? QStringLiteral("Pause (Space)")
                                     : QStringLiteral("Run (Space)"));
    if (m_playbackCard) {
        m_playbackCard->setBadge(running ? QStringLiteral("running")
                                         : QStringLiteral("paused"));
        m_playbackCard->setBadgeLive(running);
    }
}

void MainWindow::updateHud() {
    if (!m_viewport) return;
    const QVariantMap s = m_session->status();
    const int fluid = s.value(QStringLiteral("fluidCount")).toInt();
    const double spacing = s.value(QStringLiteral("spacing")).toDouble();
    const int scene = s.value(QStringLiteral("scene")).toInt();

    QStringList parts;
    if (m_perfHud && m_perfHud->isChecked()) {
        parts << QStringLiteral("%1 fps").arg(QString::number(m_viewport->fps(), 'f', 0));
        const double ms = s.value(QStringLiteral("msPerStep")).toDouble();
        if (ms > 0.01) parts << QStringLiteral("%1 ms/step").arg(ms, 0, 'f', 1);
    }
    if (m_sceneHud && m_sceneHud->isChecked()) {
        parts << (scene == 0 ? QStringLiteral("tank 2.0 × 1.2 × 1.0 m")
                             : QStringLiteral("open tank 1.0³ (inflow)"));
        parts << QStringLiteral("N=%1").arg(GroupDigits(fluid));
        parts << QStringLiteral("r=%1 m").arg(spacing, 0, 'f', 3);
    }
    m_viewport->setHudText(parts.join(QStringLiteral("   ·   ")));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
void MainWindow::setParam(const QString& key, const QVariant& value) {
    m_session->setParam(key, value);
}

void MainWindow::changeScene(int scene) {
    if (scene == m_session->params().value(QStringLiteral("scene")).toInt()) return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Rebuild scene"),
        QStringLiteral("Switching the scene resets the simulation. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        m_sceneSeg->setCurrentIndex(m_session->params().value(QStringLiteral("scene")).toInt());
        return;
    }
    setParam(QStringLiteral("scene"), scene);
}

void MainWindow::changeParticleCount(int count) {
    const int current = m_session->params().value(QStringLiteral("particleCount")).toInt();
    if (count == current) return;

    const QString budgetError = m_session->particleBudgetError(count);
    if (m_particleWarning) {
        m_particleWarning->setText(budgetError);
        m_particleWarning->setVisible(!budgetError.isEmpty());
    }
    if (!budgetError.isEmpty()) return;

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Rebuild scene"),
        QStringLiteral("Building %1 particles rebuilds the scene from scratch. Continue?")
            .arg(GroupDigits(count)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        const QSignalBlocker block(m_particles);
        m_particles->setValue(current);
        return;
    }
    setParam(QStringLiteral("particleCount"), count);
}

void MainWindow::changeSolver(int solver) {
    if (solver == m_session->params().value(QStringLiteral("solver")).toInt()) return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Switch solver"),
        QStringLiteral("The solver state differs between IISPH and DFSPH, so the scene is "
                       "restarted. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        m_solverSeg->setCurrentIndex(m_session->params().value(QStringLiteral("solver")).toInt());
        return;
    }
    setParam(QStringLiteral("solver"), solver);
}

void MainWindow::openRenderDialog() {
    if (!m_renderJob) return;
    if (m_renderJob->running()) {
        if (m_renderProgressDialog) {
            m_renderProgressDialog->show();
            m_renderProgressDialog->raise();
            m_renderProgressDialog->activateWindow();
        } else {
            QMessageBox::information(this, QStringLiteral("Render"),
                                     QStringLiteral("A render is already running."));
        }
        return;
    }
    if (m_renderOptionsDialog) {
        m_renderOptionsDialog->show();
        m_renderOptionsDialog->raise();
        m_renderOptionsDialog->activateWindow();
        return;
    }

    auto* dialog = new RenderOptionsDialog(m_session, m_renderJob, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_renderOptionsDialog = dialog;
    connect(dialog, &RenderOptionsDialog::renderStarted, this, [this] {
        if (m_renderProgressDialog) m_renderProgressDialog->close();
        auto* progress = new RenderProgressDialog(m_renderJob, this);
        progress->setAttribute(Qt::WA_DeleteOnClose);
        m_renderProgressDialog = progress;
        progress->show();
    });
    dialog->show();
}

void MainWindow::toast(const QString& text) {
    m_statusMessage = text;
    m_statusLeft->setText(text);
    const ThemeTokens& t = Theme::instance().current();
    m_statusLeft->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg((text.startsWith(QStringLiteral("Saved to ")) ? t.ok : t.fg3).name()));
    QTimer::singleShot(2600, this, [this] {
        m_statusMessage.clear();
        syncStatus();
    });
}

bool MainWindow::saveScreenshot(QString* savedPath) {
    QDir dir(QStringLiteral("captures"));
    if (!dir.exists() && !QDir().mkpath(dir.path())) {
        toast(QStringLiteral("Could not create captures/"));
        return false;
    }
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = dir.filePath(QStringLiteral("iisph-") + stamp);
    const QString imagePath = base + QStringLiteral(".png");
    if (!m_viewport->saveFrame(imagePath)) {
        toast(QStringLiteral("Screenshot failed"));
        return false;
    }
    const QString absolutePath = QDir::toNativeSeparators(QFileInfo(imagePath).absoluteFilePath());
    if (savedPath) *savedPath = absolutePath;
    toast(QStringLiteral("Saved to ") + absolutePath);
    return true;
}

void MainWindow::setThemeIndex(int index) {
    Theme::instance().setIndex(index);
}

void MainWindow::applyTheme() {
    const ThemeTokens& t = Theme::instance().current();
    if (qApp) qApp->setStyleSheet(Theme::instance().styleSheet());

    if (m_appearanceMenu) {
        const auto actions = m_appearanceMenu->actions();
        for (QAction* action : actions) {
            if (action->data().isValid()) {
                action->setChecked(action->data().toInt() == Theme::instance().index());
            }
        }
    }
    if (m_dropButton) {
        m_dropButton->setIcon(GlyphIcon(IconKind::Droplet, t.fg, 13));
    }
    if (m_particleWarning) {
        m_particleWarning->setStyleSheet(
            QStringLiteral("color: %1; font-size: 10px;").arg(t.danger.name()));
    }
    refreshTraceLegend();

    for (SidePanel* panel : {m_leftPanel, m_rightPanel}) {
        if (panel && panel->content()) panel->content()->setSpacing(t.cardGap);
    }

    const auto cards = findChildren<Card*>();
    for (Card* card : cards) {
        if (QLayout* layout = card->layout()) {
            layout->setContentsMargins(t.cardPad, 11, t.cardPad, t.cardPad);
        }
        if (t.shadow) {
            auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(card->graphicsEffect());
            if (!effect) {
                effect = new QGraphicsDropShadowEffect(card);
                effect->setBlurRadius(16);
                effect->setOffset(0, 3);
                card->setGraphicsEffect(effect);
            }
            effect->setColor(t.shadowColor);
        } else if (card->graphicsEffect()) {
            card->setGraphicsEffect(nullptr);
        }
    }

    update();
    const auto widgets = findChildren<QWidget*>();
    for (QWidget* widget : widgets) widget->update();
}

// ---------------------------------------------------------------------------
// Frameless window plumbing
// ---------------------------------------------------------------------------
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    Q_UNUSED(eventType)
    auto* msg = static_cast<MSG*>(message);
    if (msg && msg->message == WM_NCHITTEST &&
        msg->hwnd == reinterpret_cast<HWND>(winId()) && !isMaximized() && !isFullScreen()) {
        const QPoint pos = mapFromGlobal(QCursor::pos());
        if (rect().contains(pos)) {
            constexpr int margin = 7;
            const bool left = pos.x() <= margin;
            const bool right = pos.x() >= width() - margin;
            const bool top = pos.y() <= margin;
            const bool bottom = pos.y() >= height() - margin;

            LRESULT hit = 0;
            if (left && top) hit = HTTOPLEFT;
            else if (right && top) hit = HTTOPRIGHT;
            else if (left && bottom) hit = HTBOTTOMLEFT;
            else if (right && bottom) hit = HTBOTTOMRIGHT;
            else if (left) hit = HTLEFT;
            else if (right) hit = HTRIGHT;
            else if (top) hit = HTTOP;
            else if (bottom) hit = HTBOTTOM;

            if (hit != 0) {
                *result = hit;
                return true;
            }
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (isMaximized() || isFullScreen()) return;
    if (event->button() != Qt::LeftButton) return;

    const int margin = 5;
    Qt::Edges edges;
    if (event->pos().x() <= margin) edges |= Qt::LeftEdge;
    if (event->pos().x() >= width() - margin) edges |= Qt::RightEdge;
    if (event->pos().y() <= margin) edges |= Qt::TopEdge;
    if (event->pos().y() >= height() - margin) edges |= Qt::BottomEdge;

    if (edges != Qt::Edges() && windowHandle()) {
        windowHandle()->startSystemResize(edges);
    }
}

void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (isMaximized() || isFullScreen()) {
        unsetCursor();
        return;
    }
    const int margin = 5;
    Qt::Edges edges;
    if (event->pos().x() <= margin) edges |= Qt::LeftEdge;
    if (event->pos().x() >= width() - margin) edges |= Qt::RightEdge;
    if (event->pos().y() <= margin) edges |= Qt::TopEdge;
    if (event->pos().y() >= height() - margin) edges |= Qt::BottomEdge;

    if (edges == Qt::Edges()) {
        unsetCursor();
        return;
    }
    const bool left = edges & Qt::LeftEdge;
    const bool right = edges & Qt::RightEdge;
    const bool top = edges & Qt::TopEdge;
    const bool bottom = edges & Qt::BottomEdge;
    if ((left && top) || (right && bottom)) setCursor(Qt::SizeFDiagCursor);
    else if ((right && top) || (left && bottom)) setCursor(Qt::SizeBDiagCursor);
    else if (left || right) setCursor(Qt::SizeHorCursor);
    else setCursor(Qt::SizeVerCursor);
}

void MainWindow::leaveEvent(QEvent* event) {
    unsetCursor();
    QWidget::leaveEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_titleBar) {
        if (event->type() == QEvent::Resize) {
            layoutTransport();
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && windowHandle()) {
                if (me->position().y() <= 6) {
                    windowHandle()->startSystemResize(Qt::TopEdge);
                } else {
                    windowHandle()->startSystemMove();
                }
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            if (isMaximized()) showNormal();
            else showMaximized();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease && watched->property("advHeader").toBool()) {
        const bool show = !m_advancedBody->isVisible();
        m_advancedBody->setVisible(show);
        m_advancedArrow->setRotation(show ? 90.0 : 0.0);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_S) {
            saveScreenshot();
            return;
        }
        if (event->key() == Qt::Key_T) {
            Theme::instance().cycle();
            return;
        }
    }
    switch (event->key()) {
        case Qt::Key_Space:
            m_session->toggleRunning();
            return;
        case Qt::Key_R:
            m_session->resetSimulation();
            return;
        case Qt::Key_S:
            // In Fly mode S is "move backwards", not "single step".
            if (m_viewport && !m_viewport->orbitOnly()) return;
            m_session->stepOnce();
            return;
        case Qt::Key_1:
            setParam(QStringLiteral("viewMode"), 0);
            m_viewModeSeg->setCurrentIndex(0);
            m_viewport->setViewMode(0);
            return;
        case Qt::Key_2:
            setParam(QStringLiteral("viewMode"), 1);
            m_viewModeSeg->setCurrentIndex(1);
            m_viewport->setViewMode(1);
            return;
        case Qt::Key_3:
            setParam(QStringLiteral("viewMode"), 2);
            m_viewModeSeg->setCurrentIndex(2);
            m_viewport->setViewMode(2);
            return;
        case Qt::Key_F11:
            if (isFullScreen()) showNormal();
            else showFullScreen();
            return;
        case Qt::Key_Escape:
            if (isFullScreen()) showNormal();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace ui
