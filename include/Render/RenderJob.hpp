#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

class QProcess;

// Owns an offline-renderer child process. All methods and signals are used on
// the GUI thread; OpenGL stays in the child and never shares the GUI context.
class RenderJob : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(double overallProgress READ overallProgress NOTIFY overallProgressChanged)
    Q_PROPERTY(double stageProgress READ stageProgress NOTIFY stageProgressChanged)
    Q_PROPERTY(int frame READ frame NOTIFY frameChanged)
    Q_PROPERTY(int totalFrames READ totalFrames NOTIFY totalFramesChanged)
    Q_PROPERTY(double elapsedSeconds READ elapsedSeconds NOTIFY elapsedSecondsChanged)
    Q_PROPERTY(double etaSeconds READ etaSeconds NOTIFY etaSecondsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    explicit RenderJob(QObject* parent = nullptr);
    ~RenderJob() override;

    bool running() const { return m_running; }
    bool paused() const { return m_paused; }
    double overallProgress() const { return m_overallProgress; }
    double stageProgress() const { return m_stageProgress; }
    int frame() const { return m_frame; }
    int totalFrames() const { return m_totalFrames; }
    double elapsedSeconds() const { return m_elapsedSeconds; }
    double etaSeconds() const { return m_etaSeconds; }
    QString statusText() const { return m_statusText; }

    Q_INVOKABLE QVariantMap validateOptions(const QVariantMap& root) const;
    Q_INVOKABLE bool start(const QVariantMap& root);
    Q_INVOKABLE void togglePaused();
    Q_INVOKABLE void interrupt();
    Q_INVOKABLE QString urlToLocalPath(const QUrl& url) const;
    Q_INVOKABLE QString defaultOutputPath() const;

signals:
    void runningChanged();
    void pausedChanged();
    void overallProgressChanged();
    void stageProgressChanged();
    void frameChanged();
    void totalFramesChanged();
    void elapsedSecondsChanged();
    void etaSecondsChanged();
    void statusTextChanged();

    void completed();
    void failed(QString message);
    void cancelled();

private:
    void readStdout();
    void readStderr();
    void handleLine(const QByteArray& line);
    void finishFailed(const QString& message);
    void finishCancelled();
    void sendCommand(const QByteArray& command);
    void stopForDestruction();

    void setRunning(bool value);
    void setPaused(bool value);
    void setOverallProgress(double value);
    void setStageProgress(double value);
    void setFrame(int value);
    void setTotalFrames(int value);
    void setElapsedSeconds(double value);
    void setEtaSeconds(double value);
    void setStatusText(const QString& value);

    QProcess* m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QString m_stderrTail;
    QString m_stageText;
    bool m_running = false;
    bool m_paused = false;
    bool m_cancelRequested = false;
    double m_overallProgress = 0.0;
    double m_stageProgress = 0.0;
    int m_frame = 0;
    int m_totalFrames = 0;
    double m_elapsedSeconds = 0.0;
    double m_etaSeconds = -1.0;
    QString m_statusText = QStringLiteral("Idle");
};
