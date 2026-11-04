// Shared icon painter.
//
// Both front-ends draw the same 16x16 glyph set, so the paths live here rather
// than being duplicated in a QWidget::paintEvent and a QML item.
#pragma once

#include <QColor>

class QPainter;

namespace ui {

enum class IconKind {
    Play,
    Pause,
    Step,
    Reset,
    Camera,
    Fullscreen,
    Palette,
    Save,
    Chevron,
    Droplet,
    WindowMinimize,
    WindowMaximize,
    WindowClose,
};

// Draws `kind` centred on the painter's current origin in a sizePx box.
// The ink colour is taken from the active theme, so a skin switch needs no
// new pixmaps.
void PaintIcon(QPainter& painter, IconKind kind, const QColor& ink, qreal sizePx,
               qreal rotationDegrees = 0.0);

}  // namespace ui
