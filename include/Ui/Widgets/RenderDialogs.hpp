#pragma once

#include <QDialog>
#include <QVariantMap>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class RenderJob;
class SimulationSession;

namespace ui {

// Parameter editor for an offline render. It never blocks the main event loop:
// opening it is a normal modal dialog, and Start launches RenderJob's child
// process before the dialog closes.
class RenderOptionsDialog : public QDialog {
    Q_OBJECT
public:
    RenderOptionsDialog(SimulationSession* session, RenderJob* job, QWidget* parent = nullptr);

Q_SIGNALS:
    void renderStarted();

private:
    void buildUi();
    void loadValues();
    void scheduleValidation();
    void updateValidation();
    void framesFromTime();
    void timeFromFrames();
    void updateSurfaceVisibility();
    QVariantMap buildConfig() const;
    void chooseOutputDirectory();
    void startRender();

    SimulationSession* m_session = nullptr;
    RenderJob* m_job = nullptr;

    QComboBox* m_scene = nullptr;
    QComboBox* m_solver = nullptr;
    QComboBox* m_mode = nullptr;
    QSpinBox* m_particleCount = nullptr;
    QDoubleSpinBox* m_dt = nullptr;
    QDoubleSpinBox* m_omega = nullptr;
    QDoubleSpinBox* m_eta = nullptr;
    QDoubleSpinBox* m_etaV = nullptr;
    QSpinBox* m_maxIter = nullptr;
    QDoubleSpinBox* m_viscosity = nullptr;
    QDoubleSpinBox* m_tension = nullptr;
    QDoubleSpinBox* m_adhesion = nullptr;
    QGroupBox* m_surfaceGroup = nullptr;
    QDoubleSpinBox* m_grid = nullptr;
    QDoubleSpinBox* m_threshold = nullptr;
    QSpinBox* m_recon = nullptr;
    QSpinBox* m_fps = nullptr;
    QDoubleSpinBox* m_timeScale = nullptr;
    QDoubleSpinBox* m_timeSeconds = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QSpinBox* m_frames = nullptr;
    QSpinBox* m_threads = nullptr;
    QCheckBox* m_cfl = nullptr;
    QCheckBox* m_exportPly = nullptr;
    QCheckBox* m_keepFrames = nullptr;
    QCheckBox* m_noVideo = nullptr;
    QLineEdit* m_output = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_startButton = nullptr;
    QTimer* m_validateTimer = nullptr;
};

// Live two-bar progress window, modelled after Houdini's render monitor.
class RenderProgressDialog : public QDialog {
    Q_OBJECT
public:
    explicit RenderProgressDialog(RenderJob* job, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    bool confirmClose();
    void buildUi();
    void updateProgress();
    void togglePause();
    void confirmInterrupt();
    void finishCompleted();
    void finishFailed(const QString& message);
    void finishCancelled();

    RenderJob* m_job = nullptr;
    QProgressBar* m_overall = nullptr;
    QProgressBar* m_stage = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_frame = nullptr;
    QLabel* m_times = nullptr;
    QPushButton* m_pauseButton = nullptr;
    QPushButton* m_interruptButton = nullptr;
    QTimer* m_timer = nullptr;
    bool m_finished = false;
};

}  // namespace ui
