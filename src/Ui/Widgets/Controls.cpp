#include "Ui/Widgets/Controls.hpp"

#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <limits>

#include "Ui/Theme.hpp"

namespace ui {
namespace {

QFont UiFont(const QWidget* w, int px, QFont::Weight weight = QFont::Normal) {
    QFont f = w->font();
    f.setPixelSize(px);
    f.setWeight(weight);
    return f;
}

// Qt 6 returns qsizetype from container sizes; the drawing code works in int.
int Count(const QStringList& list) { return static_cast<int>(list.size()); }

}  // namespace

// ---------------------------------------------------------------------------
// SegmentedControl
// ---------------------------------------------------------------------------
SegmentedControl::SegmentedControl(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(28);
    setAccessibleName(QStringLiteral("Segmented control"));
}

void SegmentedControl::setOptions(const QStringList& options) {
    m_options = options;
    m_index = std::clamp(m_index, 0, std::max(0, Count(m_options) - 1));
    setAccessibleDescription(m_options.value(m_index));
    updateGeometry();
    update();
}

void SegmentedControl::setCurrentIndex(int index) {
    if (m_options.isEmpty()) return;
    const int clamped = std::clamp(index, 0, Count(m_options) - 1);
    if (clamped == m_index) return;
    m_index = clamped;
    setAccessibleDescription(m_options.value(m_index));
    update();
}

void SegmentedControl::setFontSize(int px) {
    m_fontSize = px;
    updateGeometry();
    update();
}

void SegmentedControl::setCompact(bool compact) {
    m_compact = compact;
    setFixedHeight(compact ? 24 : 28);
    updateGeometry();
    update();
}

QSize SegmentedControl::sizeHint() const {
    const QFontMetrics fm(UiFont(this, m_fontSize));
    int width = 0;
    for (const QString& option : m_options) {
        width += fm.horizontalAdvance(option) + (m_compact ? 14 : 22);
    }
    return QSize(width + 6, m_compact ? 24 : 28);
}

QSize SegmentedControl::minimumSizeHint() const { return sizeHint(); }

int SegmentedControl::indexAt(const QPoint& pos) const {
    if (m_options.isEmpty()) return -1;
    const qreal segW = static_cast<qreal>(width()) / m_options.size();
    const int index = static_cast<int>(pos.x() / segW);
    return std::clamp(index, 0, Count(m_options) - 1);
}

void SegmentedControl::paintEvent(QPaintEvent*) {
    const ThemeTokens& t = Theme::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal rad = t.square ? 0.0 : std::max(4.0, t.radius - 2.0);

    p.setPen(QPen(t.border, 1));
    p.setBrush(t.field);
    p.drawRoundedRect(outer, rad, rad);
    if (m_options.isEmpty()) return;

    const qreal segW = outer.width() / m_options.size();
    const QRectF selected(outer.left() + segW * m_index + 1.5, outer.top() + 1.5,
                          segW - 3.0, outer.height() - 3.0);
    const QColor selBg = t.segActive;
    const QColor selFg = t.segActiveFg;
    p.setPen(Qt::NoPen);
    p.setBrush(selBg);
    p.drawRoundedRect(selected, rad, rad);

    p.setFont(UiFont(this, m_fontSize, QFont::Medium));
    for (int i = 0; i < Count(m_options); ++i) {
        QRectF cell(outer.left() + segW * i, outer.top(), segW, outer.height());
        p.setPen(i == m_index ? selFg : (i == m_hover ? t.fg : t.fg2));
        p.drawText(cell, Qt::AlignCenter, m_options.at(i));
    }
}

void SegmentedControl::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const int index = indexAt(event->pos());
    if (index < 0 || index == m_index) return;
    m_index = index;
    setAccessibleDescription(m_options.value(m_index));
    update();
    Q_EMIT activated(index);
}

void SegmentedControl::mouseMoveEvent(QMouseEvent* event) {
    const int hover = indexAt(event->pos());
    if (hover != m_hover) {
        m_hover = hover;
        update();
    }
}

void SegmentedControl::leaveEvent(QEvent*) {
    m_hover = -1;
    update();
}

void SegmentedControl::keyPressEvent(QKeyEvent* event) {
    if (m_options.isEmpty()) return;
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const int delta = event->key() == Qt::Key_Left ? -1 : 1;
        const int next = std::clamp(m_index + delta, 0, Count(m_options) - 1);
        if (next != m_index) {
            m_index = next;
            setAccessibleDescription(m_options.value(m_index));
            update();
            Q_EMIT activated(next);
        }
        return;
    }
    QWidget::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// ToggleSwitch
// ---------------------------------------------------------------------------
ToggleSwitch::ToggleSwitch(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setFixedSize(34, 20);
    setAccessibleName(QStringLiteral("Toggle"));
}

QSize ToggleSwitch::sizeHint() const { return QSize(34, 20); }

void ToggleSwitch::setChecked(bool on) {
    if (m_on == on) return;
    m_on = on;
    setAccessibleDescription(on ? QStringLiteral("on") : QStringLiteral("off"));
    auto* anim = new QVariantAnimation(this);
    anim->setDuration(140);
    anim->setStartValue(m_pos);
    anim->setEndValue(on ? 1.0 : 0.0);
    anim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_pos = v.toReal();
        update();
    });
    connect(anim, &QVariantAnimation::finished, anim, &QObject::deleteLater);
    anim->start();
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    const ThemeTokens& t = Theme::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track = QRectF(rect()).adjusted(0.5, 2.5, -0.5, -2.5);
    const qreal rad = t.square ? 0.0 : track.height() / 2.0;
    p.setPen(Qt::NoPen);
    p.setBrush(m_on ? t.switchOn : t.track);
    p.drawRoundedRect(track, rad, rad);
    if (!m_on) {
        p.setPen(QPen(t.border, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(track, rad, rad);
    }

    const qreal knobD = track.height() - 4.0;
    const qreal travel = track.width() - knobD - 4.0;
    const QRectF knob(track.left() + 2.0 + travel * m_pos, track.top() + 2.0, knobD, knobD);
    p.setBrush(QColor(255, 255, 255));
    // Swiss uses a square white knob on a green track; without a hard outline
    // it disappears into the white card.
    p.setPen(QPen(t.square ? t.fg : QColor(0, 0, 0, 30), t.square ? 1.0 : 0.5));
    p.drawRoundedRect(knob, t.square ? 0.0 : knobD / 2.0, t.square ? 0.0 : knobD / 2.0);
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    setChecked(!m_on);
    Q_EMIT toggled(m_on);
}

void ToggleSwitch::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return) {
        setChecked(!m_on);
        Q_EMIT toggled(m_on);
        return;
    }
    QWidget::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// SliderRow
// ---------------------------------------------------------------------------
SliderRow::SliderRow(const QString& label, QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 2, 0, 2);
    outer->setSpacing(3);

    auto* head = new QHBoxLayout();
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(6);
    m_label = new QLabel(label, this);
    m_label->setProperty("role", "caption");
    m_value = new QLabel(this);
    m_value->setProperty("role", "value");
    m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    head->addWidget(m_label);
    head->addStretch(1);
    head->addWidget(m_value);
    outer->addLayout(head);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 1000);
    m_slider->setPageStep(25);
    m_slider->setFocusPolicy(Qt::StrongFocus);
    outer->addWidget(m_slider);

    connect(m_slider, &QSlider::valueChanged, this, [this](int raw) {
        refreshValueText();
        Q_EMIT valueChanged(raw / m_scale);
    });
    refreshValueText();
}

void SliderRow::setRange(double minimum, double maximum) {
    m_slider->blockSignals(true);
    m_slider->setRange(static_cast<int>(std::lround(minimum * m_scale)),
                       static_cast<int>(std::lround(maximum * m_scale)));
    m_slider->blockSignals(false);
}

void SliderRow::setValue(double value) {
    const int raw = static_cast<int>(std::lround(value * m_scale));
    if (m_slider->value() == raw) {
        refreshValueText();
        return;
    }
    m_slider->blockSignals(true);
    m_slider->setValue(raw);
    m_slider->blockSignals(false);
    refreshValueText();
}

double SliderRow::value() const { return m_slider->value() / m_scale; }

void SliderRow::setDecimals(int decimals) {
    m_decimals = decimals;
    refreshValueText();
}

void SliderRow::setSuffix(const QString& suffix) {
    m_suffix = suffix;
    refreshValueText();
}

void SliderRow::setLabelText(const QString& text) { m_label->setText(text); }

void SliderRow::refreshValueText() {
    m_value->setText(QString::number(value(), 'f', m_decimals) + m_suffix);
}

// ---------------------------------------------------------------------------
// MetricGrid
// ---------------------------------------------------------------------------
MetricGrid::MetricGrid(QWidget* parent) : QWidget(parent) {
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(11, 9, 11, 10);
    m_grid->setHorizontalSpacing(10);
    m_grid->setVerticalSpacing(8);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void MetricGrid::setMetrics(const QStringList& keys) {
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    m_tiles.clear();
    const int columns = keys.size() > 2 ? 2 : std::max(1, Count(keys));
    for (int i = 0; i < Count(keys); ++i) {
        Tile tile;
        tile.key = new QLabel(keys.at(i).toUpper(), this);
        tile.key->setProperty("role", "metricKey");
        tile.value = new QLabel(QStringLiteral("—"), this);
        tile.value->setProperty("role", "metricValue");

        auto* cell = new QWidget(this);
        auto* lay = new QVBoxLayout(cell);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(1);
        lay->addWidget(tile.key);
        lay->addWidget(tile.value);

        m_grid->addWidget(cell, i / columns, i % columns);
        m_tiles.append(tile);
    }
    for (int c = 0; c < columns; ++c) m_grid->setColumnStretch(c, 1);
}

void MetricGrid::setValue(int index, const QString& text) {
    if (index < 0 || index >= m_tiles.size()) return;
    m_tiles.at(index).value->setText(text);
}

void MetricGrid::setValueColor(int index, const QColor& color) {
    if (index < 0 || index >= m_tiles.size()) return;
    Tile& tile = m_tiles[index];
    if (tile.color == color) return;
    tile.color = color;
    // An empty style sheet falls back to the role-based colour in the theme QSS.
    tile.value->setStyleSheet(color.isValid()
                                  ? QStringLiteral("color: %1;").arg(color.name())
                                  : QString());
}

// ---------------------------------------------------------------------------
// Sparkline
// ---------------------------------------------------------------------------
Sparkline::Sparkline(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(56);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setMouseTracking(true);
}

QSize Sparkline::sizeHint() const { return QSize(200, 90); }

QColor Sparkline::BandColor(const QColor& ok, const QColor& warn, const QColor& danger,
                            double error) {
    const double e = std::abs(error);
    if (e >= kWarnError) return danger;
    if (e >= kGoodError) return warn;
    return ok;
}

void Sparkline::setSamples(const QVector<DensitySample>& samples) {
    m_samples = samples;
    if (m_hover >= m_samples.size()) m_hover = -1;
    update();
}

void Sparkline::setReferenceDensity(double density) {
    if (qFuzzyCompare(m_referenceDensity + 1.0, density + 1.0)) return;
    m_referenceDensity = density;
    update();
}

void Sparkline::clear() {
    m_samples.clear();
    m_hover = -1;
    update();
}

qreal Sparkline::xForIndex(int index, const QRectF& area) const {
    if (m_samples.size() < 2) return area.left();
    const qreal tFirst = m_samples.first().simTime;
    const qreal tLast = m_samples.last().simTime;
    const qreal timeSpan = tLast - tFirst;
    if (timeSpan <= 2.0) {
        return area.left() + area.width() * index / qMax(1, m_samples.size() - 1);
    }
    // Short traces stretch across the whole width; once they exceed 10 s the
    // window slides so the newest sample stays pinned to the right edge.
    const qreal windowSpan = std::max<qreal>(1e-6, std::min<qreal>(10.0, timeSpan));
    const qreal nx =
        std::max<qreal>(0.0, 1.0 - (tLast - m_samples.at(index).simTime) / windowSpan);
    return area.left() + area.width() * nx;
}

qreal Sparkline::yForValue(double value, qreal low, qreal high, const QRectF& area) const {
    const qreal span = std::max<qreal>(1e-6, high - low);
    const qreal norm = std::clamp((value - low) / span, 0.0, 1.0);
    return area.bottom() - norm * area.height();
}

int Sparkline::nearestIndex(int mouseX, const QRectF& area) const {
    if (m_samples.isEmpty()) return -1;
    int best = 0;
    qreal bestDistance = std::numeric_limits<qreal>::max();
    for (int i = 0; i < m_samples.size(); ++i) {
        const qreal d = std::abs(xForIndex(i, area) - mouseX);
        if (d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

void Sparkline::mouseMoveEvent(QMouseEvent* event) {
    const QRectF area = QRectF(rect()).adjusted(5.0, 14.0, -7.0, -14.0);
    const int next = nearestIndex(static_cast<int>(event->position().x()), area);
    if (next != m_hover) {
        m_hover = next;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void Sparkline::leaveEvent(QEvent* event) {
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void Sparkline::paintEvent(QPaintEvent*) {
    const ThemeTokens& t = Theme::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Scale labels live above/below the plot, not in a left-hand gutter, so
    // the trace uses the full card width.
    const QRectF area = QRectF(rect()).adjusted(5.0, 14.0, -7.0, -14.0);
    if (area.width() <= 2.0 || area.height() <= 2.0) return;

    p.setPen(QPen(t.border, 1));
    for (int i = 0; i < 3; ++i) {
        const qreal y = area.top() + area.height() * i / 2.0;
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }

    if (m_samples.size() < 2) {
        p.setFont(UiFont(this, 9));
        p.setPen(t.fg3);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("waiting for samples"));
        return;
    }

    qreal minDensity = m_samples.first().density;
    qreal maxDensity = minDensity;
    for (const DensitySample& s : m_samples) {
        minDensity = std::min(minDensity, s.density);
        maxDensity = std::max(maxDensity, s.density);
    }
    // rho0 is the reference, but it must not flatten the plot: when the mean
    // density sits far away from it, the axis follows the data and the
    // reference is reported as off-scale instead of squeezing the trace.
    const qreal dataSpan = std::max<qreal>(1e-3, maxDensity - minDensity);
    const bool includeReference = m_referenceDensity >= minDensity - dataSpan * 2.5 &&
                                  m_referenceDensity <= maxDensity + dataSpan * 2.5;
    const qreal baseMin = includeReference ? std::min(minDensity, m_referenceDensity) : minDensity;
    const qreal baseMax = includeReference ? std::max(maxDensity, m_referenceDensity) : maxDensity;
    const qreal span = std::max<qreal>(0.02, baseMax - baseMin);
    const qreal low = baseMin - span * 0.14;
    const qreal high = baseMax + span * 0.14;

    // rho0 reference: dashed rule. Its value label is painted after the trace
    // so the curve cannot run through the text.
    const bool showReference = includeReference && m_referenceDensity >= low &&
                               m_referenceDensity <= high;
    const qreal referenceY = showReference
                                 ? yForValue(m_referenceDensity, low, high, area)
                                 : 0.0;
    if (showReference) {
        p.setPen(QPen(t.fg3, 1, Qt::DashLine));
        p.drawLine(QPointF(area.left(), referenceY), QPointF(area.right(), referenceY));
    }

    // Area fill tinted by the newest sample's error band.
    QPainterPath line;
    for (int i = 0; i < m_samples.size(); ++i) {
        const qreal x = xForIndex(i, area);
        const qreal y = yForValue(m_samples.at(i).density, low, high, area);
        if (i == 0) line.moveTo(x, y);
        else line.lineTo(x, y);
    }

    QPainterPath fill = line;
    fill.lineTo(area.right(), area.bottom());
    fill.lineTo(area.left(), area.bottom());
    fill.closeSubpath();

    const QColor newest = BandColor(t.ok, t.warn, t.danger, m_samples.last().error);
    QLinearGradient grad(area.topLeft(), area.bottomLeft());
    QColor top = newest;
    top.setAlpha(46);
    QColor bottom = newest;
    bottom.setAlpha(0);
    grad.setColorAt(0.0, top);
    grad.setColorAt(1.0, bottom);

    p.setPen(Qt::NoPen);
    p.setBrush(grad);
    p.drawPath(fill);

    // The trace itself is stroked segment by segment: each piece carries the
    // error band of its own sample, so a spike stays visibly red where it
    // happened instead of tinting the whole curve.
    p.setBrush(Qt::NoBrush);
    for (int i = 1; i < m_samples.size(); ++i) {
        const DensitySample& a = m_samples.at(i - 1);
        const DensitySample& b = m_samples.at(i);
        const QColor ink = BandColor(t.ok, t.warn, t.danger, b.error);
        p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(xForIndex(i - 1, area), yForValue(a.density, low, high, area)),
                   QPointF(xForIndex(i, area), yForValue(b.density, low, high, area)));
    }

    // rho0 label on top of the trace, with an opaque veil so it stays readable
    // where the curve crosses the dashed rule.
    if (showReference) {
        const QString refText =
            QStringLiteral("ρ₀ = %1").arg(QString::number(m_referenceDensity, 'f', 0));
        p.setFont(UiFont(this, 9));
        const QFontMetricsF fm(p.font());
        QRectF label(area.left() + 2.0, referenceY - 17.0,
                     fm.horizontalAdvance(refText) + 8.0, 15.0);
        if (label.top() < area.top()) label.moveTop(referenceY + 2.0);
        p.setPen(Qt::NoPen);
        p.setBrush(t.card);
        p.drawRoundedRect(label, t.square ? 0.0 : 2.0, t.square ? 0.0 : 2.0);
        p.setPen(t.fg3);
        p.drawText(label, Qt::AlignCenter, refText);
    }

    // Scale labels sit above and below the plot; the unit is already in the
    // card header ("Mean density ... kg/m³"), so it is not repeated here.
    const qreal axisSpan = high - low;
    const int decimals = axisSpan >= 1.0 ? 2 : (axisSpan >= 0.01 ? 3 : 4);
    p.setFont(UiFont(this, 9));
    p.setPen(t.fg3);
    p.drawText(QRectF(4, 0, width() - 8, 12), Qt::AlignLeft | Qt::AlignVCenter,
               QString::number(high, 'f', decimals));
    p.drawText(QRectF(4, height() - 12, width() - 8, 12), Qt::AlignLeft | Qt::AlignVCenter,
               QString::number(low, 'f', decimals));
    if (!includeReference) {
        const QString tag = QStringLiteral("ρ₀ %1 %2")
                                .arg(QString::number(m_referenceDensity, 'f', 0),
                                     m_referenceDensity < low ? QStringLiteral("below")
                                                              : QStringLiteral("above"));
        p.drawText(QRectF(4, height() - 12, width() - 8, 12),
                   Qt::AlignRight | Qt::AlignVCenter, tag);
    }

    if (m_hover < 0 || m_hover >= m_samples.size()) return;
    const DensitySample& sample = m_samples.at(m_hover);
    const qreal hx = xForIndex(m_hover, area);
    const qreal hy = yForValue(sample.density, low, high, area);

    QColor cross = t.fg3;
    cross.setAlpha(105);
    p.setPen(QPen(cross, 1));
    p.drawLine(QPointF(hx, area.top()), QPointF(hx, area.bottom()));
    const QColor hoverInk = BandColor(t.ok, t.warn, t.danger, sample.error);
    p.setPen(QPen(t.card, 1));
    p.setBrush(hoverInk);
    p.drawEllipse(QPointF(hx, hy), 3.2, 3.2);

    const qreal tipW = 186.0;
    const qreal tipH = 66.0;
    const qreal tipX = std::clamp(hx + 10.0, area.left(), width() - tipW - 4.0);
    const QRectF tip(tipX, area.top() + 2.0, tipW, tipH);
    p.setPen(QPen(t.borderStrong, 1));
    p.setBrush(t.card);
    p.drawRoundedRect(tip, t.square ? 0.0 : t.radiusSm, t.square ? 0.0 : t.radiusSm);

    p.setFont(UiFont(this, 10));
    p.setPen(t.fg2);
    p.drawText(tip.adjusted(8, 6, -8, -tipH + 21),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("t %1 s   step %2")
                   .arg(sample.simTime, 0, 'f', 2)
                   .arg(sample.step));

    p.setFont(UiFont(this, 12, QFont::DemiBold));
    p.setPen(t.fg);
    p.drawText(tip.adjusted(8, 22, -8, -tipH + 42),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("ρ  %1 kg/m³").arg(sample.density, 0, 'f', 2));

    p.setFont(UiFont(this, 10));
    p.setPen(hoverInk);
    QString err = QString::number(std::abs(sample.error) * 100.0, 'f', 3) + QStringLiteral(" %");
    if (sample.error >= 0.0) err.prepend(QLatin1Char('+'));
    else err.prepend(QLatin1Char('-'));
    p.drawText(tip.adjusted(8, 44, -8, -3), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Δρ  ") + err);
}

// ---------------------------------------------------------------------------
// HudPill
// ---------------------------------------------------------------------------
HudPill::HudPill(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void HudPill::setText(const QString& text) {
    if (m_text == text) return;
    m_text = text;
    updateGeometry();
    update();
}

void HudPill::setDotColor(const QColor& color) {
    m_dot = color;
    update();
}

void HudPill::setDotVisible(bool visible) {
    m_dotVisible = visible;
    update();
}

QSize HudPill::sizeHint() const {
    const QFontMetrics fm(UiFont(this, 11));
    const int dot = m_dotVisible ? 12 : 0;
    return QSize(fm.horizontalAdvance(m_text) + dot + 20, 24);
}

void HudPill::paintEvent(QPaintEvent*) {
    const ThemeTokens& t = Theme::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(t.hudBorder, 1));
    p.setBrush(t.hud);
    p.drawRoundedRect(r, t.square ? 0.0 : r.height() / 2.0, t.square ? 0.0 : r.height() / 2.0);

    qreal textLeft = r.left() + 9.0;
    if (m_dotVisible) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_dot.isValid() ? m_dot : t.ok);
        p.drawEllipse(QPointF(r.left() + 11.0, r.center().y()), 3.2, 3.2);
        textLeft += 11.0;
    }
    p.setPen(QColor(0xE8, 0xEC, 0xF4));
    p.setFont(UiFont(this, 11));
    p.drawText(QRectF(textLeft, r.top(), r.right() - textLeft, r.height()),
               Qt::AlignVCenter | Qt::AlignLeft, m_text);
}

// ---------------------------------------------------------------------------
// IconButton
// ---------------------------------------------------------------------------
IconButton::IconButton(IconKind kind, QWidget* parent)
    : QAbstractButton(parent), m_kind(kind) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setFixedSize(28, 28);
}

void IconButton::setKind(IconKind kind) {
    m_kind = kind;
    update();
}

void IconButton::setIconSize(int px) {
    m_iconPx = px;
    update();
}

void IconButton::setCircular(bool circular) {
    m_circular = circular;
    update();
}

void IconButton::setDanger(bool danger) {
    m_danger = danger;
    update();
}

void IconButton::setPrimary(bool primary) {
    m_primary = primary;
    update();
}

void IconButton::setRotation(qreal degrees) {
    m_rotation = degrees;
    update();
}

QSize IconButton::sizeHint() const { return QSize(28, 28); }

void IconButton::enterEvent(QEnterEvent*) {
    m_hover = true;
    update();
}

void IconButton::leaveEvent(QEvent*) {
    m_hover = false;
    update();
}

void IconButton::paintEvent(QPaintEvent*) {
    const ThemeTokens& t = Theme::instance().current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = m_circular ? box.height() / 2.0 : t.radiusSm;
    if (m_primary) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_hover ? t.accent.lighter(112) : t.accent);
        p.drawRoundedRect(box, radius, radius);
    } else if (m_hover || isChecked()) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_danger && m_hover ? t.danger
                                       : (isChecked() ? t.accentSoft : t.chip));
        p.drawRoundedRect(box, radius, radius);
    }

    const QColor ink = m_primary ? t.accentFg
                     : (!isEnabled() ? t.fg3
                     : (m_danger && m_hover ? QColor(Qt::white)
                     : (isChecked() ? t.accent : (m_hover ? t.fg : t.fg2))));
    p.translate(width() / 2.0, height() / 2.0);
    PaintIcon(p, m_kind, ink, m_iconPx, m_rotation);
}

}  // namespace ui
