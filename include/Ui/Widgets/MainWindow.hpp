// MainWindow — the widget front-end.
//
// Frameless: the title bar is drawn by the app (drag to move, double click to
// maximise, 5 px edge hit-test for resizing). Left panel runs and reports, right
// panel edits parameters, the viewport sits in between.
#pragma once

#include <QPoint>
#include <QWidget>

class QComboBox;
class QLabel;
class QMenu;
class QPushButton;
class SimulationSession;

namespace ui {

class Card;
class IconButton;
class MetricGrid;
class SegmentedControl;
class SidePanel;
class SliderRow;
class Sparkline;
class ToggleSwitch;
class Viewport;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(SimulationSession* session, QWidget* parent = nullptr);
    ~MainWindow() override;

    void applyTheme();
    void setThemeIndex(int index);
    Viewport* viewport() const { return m_viewport; }

    // Renders one frame and writes it to captures/ (PNG + matching JSON).
    bool saveScreenshot(QString* savedPath = nullptr);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void refreshTraceLegend();
    QWidget* buildTitleBar();
    QWidget* buildMenuBar();
    void layoutTransport();
    void buildLeftPanel();
    void buildRightPanel();
    QWidget* buildStatusBar();
    void wire();

    void syncParams();
    void syncStatus();
    void syncTrace();
    void updateTransport();
    void updateHud();

    void setParam(const QString& key, const QVariant& value);
    void changeScene(int scene);
    void changeResolution(int index);
    void changeSolver(int solver);
    void toast(const QString& text);

    SimulationSession* m_session = nullptr;
    Viewport* m_viewport = nullptr;
    SidePanel* m_leftPanel = nullptr;
    SidePanel* m_rightPanel = nullptr;

    // title bar
    QWidget* m_titleBar = nullptr;
    IconButton* m_resetButton = nullptr;
    IconButton* m_playButton = nullptr;
    IconButton* m_stepButton = nullptr;
    IconButton* m_saveButton = nullptr;
    IconButton* m_fullscreenButton = nullptr;
    IconButton* m_minimizeButton = nullptr;
    IconButton* m_maximizeButton = nullptr;
    IconButton* m_closeButton = nullptr;
    QLabel* m_clock = nullptr;
    SegmentedControl* m_viewModeSeg = nullptr;
    QMenu* m_appearanceMenu = nullptr;

    // left panel
    SegmentedControl* m_sceneSeg = nullptr;
    Card* m_playbackCard = nullptr;
    QComboBox* m_resolution = nullptr;
    QLabel* m_particleCount = nullptr;
    QPushButton* m_dropButton = nullptr;
    SliderRow* m_speed = nullptr;
    QLabel* m_actualRate = nullptr;
    QLabel* m_stepCost = nullptr;
    MetricGrid* m_metrics = nullptr;
    QLabel* m_traceValue = nullptr;
    QLabel* m_traceLegend = nullptr;
    Sparkline* m_spark = nullptr;

    // right panel
    SegmentedControl* m_solverSeg = nullptr;
    IconButton* m_advancedArrow = nullptr;
    QWidget* m_advancedBody = nullptr;
    SliderRow* m_omega = nullptr;
    SliderRow* m_eta = nullptr;
    SliderRow* m_etaV = nullptr;
    SliderRow* m_maxIter = nullptr;
    SliderRow* m_dt = nullptr;
    ToggleSwitch* m_cfl = nullptr;
    SliderRow* m_viscosity = nullptr;
    SliderRow* m_tension = nullptr;
    SliderRow* m_adhesion = nullptr;
    SliderRow* m_threshold = nullptr;
    SliderRow* m_gridScale = nullptr;
    SliderRow* m_reconInterval = nullptr;
    SegmentedControl* m_cameraSeg = nullptr;
    SliderRow* m_fov = nullptr;
    ToggleSwitch* m_perfHud = nullptr;
    ToggleSwitch* m_sceneHud = nullptr;

    // status bar
    QLabel* m_statusLeft = nullptr;
    QLabel* m_statusRight = nullptr;
    QString m_statusMessage;

    bool m_syncing = false;
};

}  // namespace ui
