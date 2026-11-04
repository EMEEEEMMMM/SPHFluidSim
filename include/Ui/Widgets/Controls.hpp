// Small hand-drawn controls for the widget front-end.
//
// Qt style sheets cannot express a segmented control, an iOS-style switch or a
// sparkline, and coaxing QTabBar/QCheckBox into those shapes looks wrong at the
// sizes this UI uses. These four are painted directly from the theme tokens so
// all four skins stay consistent with the QML front-end.
#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "Ui/Icons.hpp"

class QLabel;
class QSlider;
class QGridLayout;

namespace ui {

class SegmentedControl : public QWidget {
    Q_OBJECT
public:
    explicit SegmentedControl(QWidget* parent = nullptr);

    void setOptions(const QStringList& options);
    int currentIndex() const { return m_index; }
    void setCurrentIndex(int index);
    void setFontSize(int px);
    void setCompact(bool compact);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void activated(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    int indexAt(const QPoint& pos) const;

    QStringList m_options;
    int m_index = 0;
    int m_hover = -1;
    int m_fontSize = 11;
    bool m_compact = false;
};

class ToggleSwitch : public QWidget {
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    bool isChecked() const { return m_on; }
    void setChecked(bool on);

    QSize sizeHint() const override;

Q_SIGNALS:
    void toggled(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    bool m_on = false;
    qreal m_pos = 0.0;   // animated knob position, 0 = off, 1 = on
};

// Label + right-aligned value + slider underneath.
class SliderRow : public QWidget {
    Q_OBJECT
public:
    explicit SliderRow(const QString& label, QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    void setValue(double value);
    double value() const;
    void setDecimals(int decimals);
    void setSuffix(const QString& suffix);
    void setLabelText(const QString& text);

Q_SIGNALS:
    void valueChanged(double value);

private:
    void refreshValueText();

    QLabel* m_label = nullptr;
    QLabel* m_value = nullptr;
    QSlider* m_slider = nullptr;
    QString m_suffix;
    int m_decimals = 3;
    double m_scale = 1000.0;
};

class MetricGrid : public QWidget {
    Q_OBJECT
public:
    explicit MetricGrid(QWidget* parent = nullptr);

    // Keys are shown in upper case above the value.
    void setMetrics(const QStringList& keys);
    void setValue(int index, const QString& text);
    // Optional per-cell colour (density-error banding); pass an invalid QColor
    // to fall back to the theme's regular value colour.
    void setValueColor(int index, const QColor& color);

private:
    struct Tile {
        QLabel* key = nullptr;
        QLabel* value = nullptr;
        QColor color;
    };
    QVector<Tile> m_tiles;
    QGridLayout* m_grid = nullptr;
};

struct DensitySample {
    double simTime = 0.0;
    double density = 1000.0;
    double error = 0.0;
    qulonglong step = 0;
};

// Mean-density sparkline: real kg/m^3 values, a labelled rho0 reference and a
// hover readout. The curve is coloured by density error so the reader can see
// the constraint being held (green), strained (amber) or lost (red).
class Sparkline : public QWidget {
    Q_OBJECT
public:
    explicit Sparkline(QWidget* parent = nullptr);

    // Error bands as fractions: 0.001 = 0.10 %, 0.010 = 1.00 %. A converged
    // run sits near 0.010 %, an order of magnitude inside the green band.
    static constexpr double kGoodError = 0.001;
    static constexpr double kWarnError = 0.010;

    static QColor BandColor(const QColor& ok, const QColor& warn, const QColor& danger,
                            double error);

    void setSamples(const QVector<DensitySample>& samples);
    void setReferenceDensity(double density);
    void clear();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int nearestIndex(int mouseX, const QRectF& area) const;
    qreal xForIndex(int index, const QRectF& area) const;
    qreal yForValue(double value, qreal low, qreal high, const QRectF& area) const;

    QVector<DensitySample> m_samples;
    double m_referenceDensity = 1000.0;
    int m_hover = -1;
};

// Round pill showing FPS / tank label over the viewport. Painted, not a QLabel,
// so it keeps the translucent HUD look on top of the GL surface.
class HudPill : public QWidget {
    Q_OBJECT
public:
    explicit HudPill(QWidget* parent = nullptr);

    void setText(const QString& text);
    void setDotColor(const QColor& color);
    void setDotVisible(bool visible);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_text;
    QColor m_dot;
    bool m_dotVisible = false;
};

// Icon button painted from the theme, so a skin switch never leaves a stale
// coloured pixmap behind.
class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit IconButton(IconKind kind, QWidget* parent = nullptr);

    void setKind(IconKind kind);
    void setIconSize(int px);
    void setCircular(bool circular);
    void setDanger(bool danger);
    void setPrimary(bool primary);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    IconKind m_kind;
    int m_iconPx = 16;
    bool m_circular = false;
    bool m_hover = false;
    bool m_danger = false;
    bool m_primary = false;
    qreal m_rotation = 0.0;
public:
    void setRotation(qreal degrees);
};

}  // namespace ui
