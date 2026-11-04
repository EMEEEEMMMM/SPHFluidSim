// IconItem — paints the shared 16x16 glyph set inside QML.
#pragma once

#include <QColor>
#include <QQuickPaintedItem>

namespace ui {

class IconItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(int kind READ kind WRITE setKind NOTIFY kindChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal angle READ angle WRITE setAngle NOTIFY angleChanged)

public:
    explicit IconItem(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    int kind() const { return m_kind; }
    void setKind(int kind);

    QColor color() const { return m_color; }
    void setColor(const QColor& color);

    qreal angle() const { return m_angle; }
    void setAngle(qreal angle);

Q_SIGNALS:
    void kindChanged();
    void colorChanged();
    void angleChanged();

private:
    int m_kind = 0;
    QColor m_color;
    qreal m_angle = 0.0;
};

}  // namespace ui
