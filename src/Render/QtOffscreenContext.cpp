#include "Render/QtOffscreenContext.hpp"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

namespace render {

struct QtOffscreenContext::Impl {
    std::unique_ptr<QOffscreenSurface> surface;
    std::unique_ptr<QOpenGLContext> context;
};

QtOffscreenContext::QtOffscreenContext() : m_impl(std::make_unique<Impl>()) {}
QtOffscreenContext::~QtOffscreenContext() { Reset(); }

bool QtOffscreenContext::Create() {
    if (IsValid()) return true;
    if (!QGuiApplication::instance()) return false;

    Reset();

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);

    m_impl->surface = std::make_unique<QOffscreenSurface>();
    m_impl->surface->setFormat(format);
    m_impl->surface->create();
    if (!m_impl->surface->isValid()) {
        Reset();
        return false;
    }

    m_impl->context = std::make_unique<QOpenGLContext>();
    m_impl->context->setFormat(format);
    if (!m_impl->context->create() || !m_impl->context->makeCurrent(m_impl->surface.get())) {
        Reset();
        return false;
    }
    return true;
}

bool QtOffscreenContext::IsValid() const {
    return m_impl && m_impl->surface && m_impl->context && m_impl->context->isValid();
}

void QtOffscreenContext::Reset() {
    if (!m_impl) return;
    if (m_impl->context) {
        if (QOpenGLContext::currentContext() == m_impl->context.get()) {
            m_impl->context->doneCurrent();
        }
        m_impl->context.reset();
    }
    m_impl->surface.reset();
}

void* QtOffscreenContext::ProcAddress(const char* name) {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    return context ? reinterpret_cast<void*>(context->getProcAddress(name)) : nullptr;
}

}  // namespace render
