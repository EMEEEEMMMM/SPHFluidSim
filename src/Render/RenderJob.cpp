#include "Render/RenderJob.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

QString LocalPath(const QVariant& value) {
    if (value.metaType().id() == QMetaType::QUrl)
        return value.toUrl().toLocalFile();
    const QString text = value.toString().trimmed();
    const QUrl url(text);
    if (url.isValid() && url.isLocalFile()) return url.toLocalFile();
    return text;
}

bool FiniteNumber(const QVariantMap& map, const QString& key, double& value) {
    bool ok = false;
    value = map.value(key).toDouble(&ok);
    return ok && std::isfinite(value);
}

QString FormatBytes(qulonglong bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(size, 0, 'f', unit == 0 ? 0 : 2)
        .arg(QString::fromLatin1(units[unit]));
}

QVariantMap ValidationResult(bool valid, const QString& message,
                             qulonglong available = 0, qulonglong required = 0) {
    return {
        {QStringLiteral("valid"), valid},
        {QStringLiteral("message"), message},
        {QStringLiteral("availableBytes"), QVariant::fromValue<qulonglong>(available)},
        {QStringLiteral("requiredBytes"), QVariant::fromValue<qulonglong>(required)}
    };
}

double Clamped01(const QJsonObject& object, const char* key, double oldValue) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isDouble()) return oldValue;
    return std::clamp(value.toDouble(), 0.0, 1.0);
}

} // namespace

RenderJob::RenderJob(QObject* parent) : QObject(parent), m_process(new QProcess(this)) {
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &RenderJob::readStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &RenderJob::readStderr);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_cancelRequested) return;
        if (error == QProcess::FailedToStart) {
            finishFailed(tr("Could not start the renderer: %1").arg(m_process->errorString()));
        } else if (error == QProcess::Crashed && m_running) {
            finishFailed(tr("The renderer stopped unexpectedly: %1").arg(m_process->errorString()));
        }
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (!m_running) return;
                readStdout();
                readStderr();
                if (m_cancelRequested) {
                    finishCancelled();
                    return;
                }
                if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                    QString message = tr("Rendering failed (exit code %1).").arg(exitCode);
                    if (!m_stderrTail.trimmed().isEmpty())
                        message += QLatin1Char('\n') + m_stderrTail.trimmed();
                    finishFailed(message);
                    return;
                }
                setOverallProgress(1.0);
                setStageProgress(1.0);
                setFrame(m_totalFrames);
                setRunning(false);
                setPaused(false);
                setStatusText(tr("Completed"));
                emit completed();
            });
}

RenderJob::~RenderJob() {
    disconnect(m_process, nullptr, this, nullptr);
    stopForDestruction();
}

QString RenderJob::urlToLocalPath(const QUrl& url) const {
    return url.isLocalFile() ? url.toLocalFile() : QString();
}

QString RenderJob::defaultOutputPath() const {
    QStringList candidates;
    candidates << QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
               << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
               << QDir::homePath();
    QString base;
    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isDir()) {
            base = candidate;
            break;
        }
    }
    if (base.isEmpty()) base = QDir::homePath();
    return QDir::cleanPath(QDir(base).filePath(QStringLiteral("IISPH/render")));
}

QVariantMap RenderJob::validateOptions(const QVariantMap& root) const {
    if (!root.value(QStringLiteral("app")).toString().isEmpty() &&
        root.value(QStringLiteral("app")).toString() != QLatin1String("IISPH"))
        return ValidationResult(false, tr("The configuration is not an IISPH render job."));
    if (root.contains(QStringLiteral("schemaVersion")) &&
        root.value(QStringLiteral("schemaVersion")).toInt() != 1)
        return ValidationResult(false, tr("Unsupported configuration schema version."));

    const QVariantMap simulation = root.value(QStringLiteral("simulation")).toMap();
    const QVariantMap render = root.value(QStringLiteral("render")).toMap();
    const QString scene = simulation.value(QStringLiteral("scene"), QStringLiteral("dam")).toString();
    if (scene != QLatin1String("dam") && scene != QLatin1String("inflow"))
        return ValidationResult(false, tr("Scene must be 'dam' or 'inflow'."));
    const QString solver = simulation.value(QStringLiteral("solver"), QStringLiteral("IISPH")).toString();
    if (solver.compare(QLatin1String("IISPH"), Qt::CaseInsensitive) != 0 &&
        solver.compare(QLatin1String("DFSPH"), Qt::CaseInsensitive) != 0)
        return ValidationResult(false, tr("Solver must be 'IISPH' or 'DFSPH'."));

    double value = 0.0;
    if (simulation.contains(QStringLiteral("particleCount")) &&
        (!FiniteNumber(simulation, QStringLiteral("particleCount"), value) || value <= 0.0))
        return ValidationResult(false, tr("Particle count must be positive."));
    const auto positiveSimulation = [&](const char* key, bool allowZero = false) -> QVariantMap {
        const QString name = QLatin1String(key);
        if (!simulation.contains(name)) return {};
        if (!FiniteNumber(simulation, name, value) || (allowZero ? value < 0.0 : value <= 0.0))
            return ValidationResult(false, tr("Simulation parameter '%1' has an invalid value.").arg(name));
        return {};
    };
    for (const char* key : {"spacing", "omega", "eta", "etaV"}) {
        const QVariantMap result = positiveSimulation(key);
        if (!result.isEmpty()) return result;
    }
    for (const char* key : {"dt", "viscosity", "surfaceTension", "wallAdhesion"}) {
        const QVariantMap result = positiveSimulation(key, true);
        if (!result.isEmpty()) return result;
    }
    if (simulation.contains(QStringLiteral("maxIter")) &&
        simulation.value(QStringLiteral("maxIter")).toInt() <= 0)
        return ValidationResult(false, tr("Maximum solver iterations must be positive."));

    const QString mode = render.value(QStringLiteral("mode"), QStringLiteral("surface")).toString();
    if (mode != QLatin1String("particles") && mode != QLatin1String("surface"))
        return ValidationResult(false, tr("Render mode must be 'particles' or 'surface'."));

    int fps = 30;
    int frames = 300;
    int width = 1280;
    int height = 720;
    double seconds = 0.0;
    double timeScale = 1.0;
    if (render.contains(QStringLiteral("fps"))) {
        if (!FiniteNumber(render, QStringLiteral("fps"), value) || value <= 0.0 || value > 1000.0)
            return ValidationResult(false, tr("FPS must be in the range 1..1000."));
        fps = static_cast<int>(value);
    }
    if (render.contains(QStringLiteral("seconds"))) {
        if (!FiniteNumber(render, QStringLiteral("seconds"), seconds) || seconds <= 0.0)
            return ValidationResult(false, tr("Render duration must be positive."));
        frames = static_cast<int>(seconds * fps + 0.5);
    } else if (render.contains(QStringLiteral("frames"))) {
        if (!FiniteNumber(render, QStringLiteral("frames"), value) || value <= 0.0)
            return ValidationResult(false, tr("Frame count must be positive."));
        frames = static_cast<int>(value);
    }
    if (frames <= 0) return ValidationResult(false, tr("The render must contain at least one frame."));
    if (render.contains(QStringLiteral("timeScale")) &&
        (!FiniteNumber(render, QStringLiteral("timeScale"), timeScale) || timeScale <= 0.0))
        return ValidationResult(false, tr("Time scale must be positive."));
    if (render.contains(QStringLiteral("width"))) width = render.value(QStringLiteral("width")).toInt();
    if (render.contains(QStringLiteral("height"))) height = render.value(QStringLiteral("height")).toInt();
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return ValidationResult(false, tr("Resolution must be positive and no larger than 16384 pixels per axis."));
    if (render.contains(QStringLiteral("gridScale")) &&
        (!FiniteNumber(render, QStringLiteral("gridScale"), value) || value <= 0.0 || value > 4.0))
        return ValidationResult(false, tr("Surface grid scale must be in the range 0..4."));
    if (render.contains(QStringLiteral("surfaceThreshold")) &&
        (!FiniteNumber(render, QStringLiteral("surfaceThreshold"), value) || value < 0.0))
        return ValidationResult(false, tr("Surface threshold must be non-negative and finite."));
    if (render.contains(QStringLiteral("reconInterval")) &&
        render.value(QStringLiteral("reconInterval")).toInt() <= 0)
        return ValidationResult(false, tr("Surface rebuild interval must be at least one frame."));
    if (render.contains(QStringLiteral("threads")) && render.value(QStringLiteral("threads")).toInt() < 0)
        return ValidationResult(false, tr("Thread count cannot be negative."));

    const QString output = LocalPath(render.value(QStringLiteral("output")));
    if (output.trimmed().isEmpty())
        return ValidationResult(false, tr("Choose an output directory before rendering."));
    const QFileInfo outputInfo(output);
    if (outputInfo.exists() && !outputInfo.isDir())
        return ValidationResult(false, tr("The output path points to a file, not a directory."));

    QString nearest = outputInfo.isDir() ? outputInfo.absoluteFilePath()
                                         : outputInfo.absolutePath();
    nearest = QDir::cleanPath(nearest);
    while (!QFileInfo::exists(nearest)) {
        const QString parent = QFileInfo(nearest).absolutePath();
        if (parent.isEmpty() || parent == nearest)
            return ValidationResult(false, tr("No existing parent directory could be found for the output path."));
        nearest = parent;
    }
    QStorageInfo volume(nearest);
    volume.refresh();
    if (!volume.isValid() || !volume.isReady() || volume.bytesAvailable() < 0)
        return ValidationResult(false, tr("Free space on the output volume is unavailable."));
    const qulonglong available = static_cast<qulonglong>(volume.bytesAvailable());

    const int particles = simulation.value(QStringLiteral("particleCount"), 8000).toInt();
    long double requiredEstimate = 0.0;
    if (render.value(QStringLiteral("exportParticles"), false).toBool()) {
        // Match HeadlessRunner's binary PLY estimate: 12 bytes/vertex, a small
        // header, 30% more particles for drops, and 2% container overhead.
        const long double n = static_cast<long double>(particles) * 1.3L + 1024.0L;
        requiredEstimate += (200.0L + n * 12.0L) * frames * 1.02L;
    }
    const bool noVideo = render.value(QStringLiteral("noVideo"), false).toBool();
    const bool keepFrames = render.value(QStringLiteral("keepFrames"), false).toBool();
    if (noVideo || keepFrames) {
        const long double rowSize = ((static_cast<long double>(width) * 3.0L) + 3.0L);
        const long double alignedRow = static_cast<qulonglong>(rowSize) & ~3ull;
        requiredEstimate += (54.0L + alignedRow * height) * frames;
    }
    if (!noVideo) {
        // A modest H.264 allowance plus muxing/temporary-file headroom.
        const long double videoBytes = std::max<long double>(64.0L * 1024.0L * 1024.0L,
            (static_cast<long double>(frames) / fps) * 2.0L * 1024.0L * 1024.0L);
        requiredEstimate += videoBytes;
    }
    const long double maxQulonglong = static_cast<long double>(std::numeric_limits<qulonglong>::max());
    const qulonglong required = requiredEstimate >= maxQulonglong
        ? std::numeric_limits<qulonglong>::max()
        : static_cast<qulonglong>(std::ceil(requiredEstimate));
    if (required > available) {
        return ValidationResult(false,
            tr("Not enough free space on %1: about %2 is required, but only %3 is available.")
                .arg(volume.rootPath(), FormatBytes(required), FormatBytes(available)),
            available, required);
    }
    return ValidationResult(true,
        tr("Output is ready. Estimated space: %1; available on %2: %3.")
            .arg(FormatBytes(required), volume.rootPath(), FormatBytes(available)),
        available, required);
}

bool RenderJob::start(const QVariantMap& root) {
    if (m_running) {
        emit failed(tr("A render is already running."));
        return false;
    }
    const QVariantMap validation = validateOptions(root);
    if (!validation.value(QStringLiteral("valid")).toBool()) {
        emit failed(validation.value(QStringLiteral("message")).toString());
        return false;
    }

    QVariantMap actual = root;
    actual.insert(QStringLiteral("app"), QStringLiteral("IISPH"));
    actual.insert(QStringLiteral("schemaVersion"), 1);
    QVariantMap render = actual.value(QStringLiteral("render")).toMap();
    const QString output = QFileInfo(LocalPath(render.value(QStringLiteral("output")))).absoluteFilePath();
    render.insert(QStringLiteral("output"), output);
    const int fps = render.value(QStringLiteral("fps"), 30).toInt();
    if (render.contains(QStringLiteral("seconds"))) {
        const int frames = static_cast<int>(render.value(QStringLiteral("seconds")).toDouble() * fps + 0.5);
        render.insert(QStringLiteral("frames"), frames);
    }
    actual.insert(QStringLiteral("render"), render);

    QDir outputDir(output);
    if (!outputDir.mkpath(QStringLiteral("."))) {
        emit failed(tr("Could not create the output directory: %1").arg(output));
        return false;
    }
    QSaveFile configFile(outputDir.filePath(QStringLiteral("config.json")));
    if (!configFile.open(QIODevice::WriteOnly)) {
        emit failed(tr("Could not write %1: %2").arg(configFile.fileName(), configFile.errorString()));
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromVariant(actual);
    if (configFile.write(document.toJson(QJsonDocument::Indented)) < 0 || !configFile.commit()) {
        emit failed(tr("Could not write %1: %2").arg(configFile.fileName(), configFile.errorString()));
        return false;
    }

    m_cancelRequested = false;
    m_stdoutBuffer.clear();
    m_stderrTail.clear();
    m_stageText.clear();
    setOverallProgress(0.0);
    setStageProgress(0.0);
    setFrame(0);
    setTotalFrames(render.value(QStringLiteral("frames"), 300).toInt());
    setElapsedSeconds(0.0);
    setEtaSeconds(-1.0);
    setPaused(false);
    setStatusText(tr("Starting"));
    setRunning(true);

    m_process->setProgram(QCoreApplication::applicationFilePath());
    m_process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_process->setArguments({QStringLiteral("--headless"), QStringLiteral("--control"),
                             QStringLiteral("--config"), configFile.fileName()});
    m_process->start(QIODevice::ReadWrite);
    return true;
}

void RenderJob::togglePaused() {
    if (!m_running || m_cancelRequested) return;
    sendCommand(m_paused ? QByteArray("resume\n") : QByteArray("pause\n"));
}

void RenderJob::interrupt() {
    if (!m_running || m_cancelRequested) return;
    m_cancelRequested = true;
    setStatusText(tr("Cancelling"));
    sendCommand(QByteArray("cancel\n"));
}

void RenderJob::sendCommand(const QByteArray& command) {
    if (m_process->state() == QProcess::NotRunning || !m_process->isWritable()) return;
    m_process->write(command);
}

void RenderJob::readStdout() {
    m_stdoutBuffer += m_process->readAllStandardOutput();
    int newline = -1;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        QByteArray line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        handleLine(line);
    }
}

void RenderJob::readStderr() {
    m_stderrTail += QString::fromLocal8Bit(m_process->readAllStandardError());
    if (m_stderrTail.size() > 4000) m_stderrTail.remove(0, m_stderrTail.size() - 4000);
}

void RenderJob::handleLine(const QByteArray& line) {
    const bool progress = line.startsWith("@PROGRESS ");
    const bool state = line.startsWith("@STATE ");
    if (!progress && !state) return;
    const QJsonDocument document = QJsonDocument::fromJson(line.mid(progress ? 10 : 7));
    if (!document.isObject()) return;
    const QJsonObject object = document.object();
    if (progress) {
        setFrame(object.value(QStringLiteral("frame")).toInt(m_frame));
        setTotalFrames(object.value(QStringLiteral("totalFrames")).toInt(m_totalFrames));
        setOverallProgress(Clamped01(object, "overall", m_overallProgress));
        setStageProgress(Clamped01(object, "stageProgress", m_stageProgress));
        const double elapsed = object.value(QStringLiteral("elapsed")).toDouble(m_elapsedSeconds);
        const double eta = object.value(QStringLiteral("eta")).toDouble(m_etaSeconds);
        if (std::isfinite(elapsed) && elapsed >= 0.0) setElapsedSeconds(elapsed);
        if (std::isfinite(eta)) setEtaSeconds(eta);
        const QString stage = object.value(QStringLiteral("stage")).toString();
        if (!stage.isEmpty()) {
            m_stageText = stage;
            if (!m_paused) setStatusText(stage);
        }
        return;
    }

    const QString value = object.value(QStringLiteral("state")).toString();
    if (value == QLatin1String("paused")) {
        setPaused(true);
        setStatusText(tr("Paused"));
    } else if (value == QLatin1String("running")) {
        setPaused(false);
        if (!m_stageText.isEmpty()) setStatusText(m_stageText);
    } else if (value == QLatin1String("cancelled")) {
        setStatusText(tr("Cancelling"));
    } else if (value == QLatin1String("failed") && m_running && !m_cancelRequested) {
        finishFailed(object.value(QStringLiteral("message")).toString(tr("Rendering failed.")));
    }
}

void RenderJob::finishFailed(const QString& message) {
    if (!m_running) return;
    setRunning(false);
    setPaused(false);
    setStatusText(tr("Failed"));
    emit failed(message);
}

void RenderJob::finishCancelled() {
    if (!m_running) return;
    setRunning(false);
    setPaused(false);
    setStatusText(tr("Cancelled"));
    emit cancelled();
}

void RenderJob::stopForDestruction() {
    if (m_process->state() == QProcess::NotRunning) return;
    m_cancelRequested = true;
    sendCommand(QByteArray("cancel\n"));
    if (m_process->waitForFinished(2500)) return;
    m_process->terminate();
    if (m_process->waitForFinished(1000)) return;
    m_process->kill();
    m_process->waitForFinished();
}

void RenderJob::setRunning(bool value) {
    if (m_running == value) return;
    m_running = value;
    emit runningChanged();
}
void RenderJob::setPaused(bool value) {
    if (m_paused == value) return;
    m_paused = value;
    emit pausedChanged();
}
void RenderJob::setOverallProgress(double value) {
    if (m_overallProgress == value) return;
    m_overallProgress = value;
    emit overallProgressChanged();
}
void RenderJob::setStageProgress(double value) {
    if (m_stageProgress == value) return;
    m_stageProgress = value;
    emit stageProgressChanged();
}
void RenderJob::setFrame(int value) {
    if (m_frame == value) return;
    m_frame = value;
    emit frameChanged();
}
void RenderJob::setTotalFrames(int value) {
    if (m_totalFrames == value) return;
    m_totalFrames = value;
    emit totalFramesChanged();
}
void RenderJob::setElapsedSeconds(double value) {
    if (m_elapsedSeconds == value) return;
    m_elapsedSeconds = value;
    emit elapsedSecondsChanged();
}
void RenderJob::setEtaSeconds(double value) {
    if (m_etaSeconds == value) return;
    m_etaSeconds = value;
    emit etaSecondsChanged();
}
void RenderJob::setStatusText(const QString& value) {
    if (m_statusText == value) return;
    m_statusText = value;
    emit statusTextChanged();
}
