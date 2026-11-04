// Card + scrolling side panel: the two containers every parameter group lives in.
#pragma once

#include <QFrame>
#include <QScrollArea>

class QLabel;
class QVBoxLayout;

namespace ui {

class Card : public QFrame {
    Q_OBJECT
public:
    explicit Card(const QString& title, QWidget* parent = nullptr);

    QVBoxLayout* body() const { return m_body; }
    void setBadge(const QString& text);
    void setBadgeLive(bool live);
    void addWidget(QWidget* widget);

    // "Label            value" row; returns the value label so callers can keep
    // updating it (particle counts, iteration counts, ...).
    QLabel* addValueRow(const QString& caption, const QString& value = QStringLiteral("—"));
    void addSpacing(int pixels);

private:
    QVBoxLayout* m_body = nullptr;
    QLabel* m_badge = nullptr;
};

class SidePanel : public QScrollArea {
    Q_OBJECT
public:
    explicit SidePanel(QWidget* parent = nullptr);

    QVBoxLayout* content() const { return m_content; }
    Card* addCard(const QString& title);
    void addStretch();

private:
    QWidget* m_host = nullptr;
    QVBoxLayout* m_content = nullptr;
};

}  // namespace ui
