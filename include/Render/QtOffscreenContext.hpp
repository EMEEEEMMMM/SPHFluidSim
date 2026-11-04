// Qt-owned offscreen OpenGL context for headless rendering.
//
// This interface deliberately contains no Qt or OpenGL types. The implementation
// lives in a Qt-only translation unit so it never collides with glad's GL headers.
#pragma once

#include <memory>

namespace render {

class QtOffscreenContext {
public:
    QtOffscreenContext();
    ~QtOffscreenContext();

    QtOffscreenContext(const QtOffscreenContext&) = delete;
    QtOffscreenContext& operator=(const QtOffscreenContext&) = delete;

    // Requires an existing QGuiApplication/QApplication. Leaves a GL 3.3 core
    // context current on success and releases all resources on failure.
    bool Create();
    bool IsValid() const;

    // GLAD loader for the context that is current on the calling thread.
    static void* ProcAddress(const char* name);

private:
    void Reset();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace render
