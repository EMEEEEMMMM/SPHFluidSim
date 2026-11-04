#pragma once

#include <string>

class QApplication;
class SimulationSession;

namespace ui {

// Starts the Qt Quick front-end. Returns the process exit code.
int RunQmlFrontend(QApplication& app, SimulationSession& session, bool autoplay,
                   const std::string& screenshotPath, int screenshotDelayMs);

}  // namespace ui
