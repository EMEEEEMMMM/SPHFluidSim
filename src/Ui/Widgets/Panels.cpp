#include "Ui/Widgets/Panels.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

#include "Ui/Theme.hpp"

namespace ui {

Card::Card(const QString& title, QWidget* parent) : QFrame(parent) {
    setProperty("card", "true");
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 10, 12, 12);
    outer->setSpacing(8);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);

    auto* titleLabel = new QLabel(title.toUpper(), this);
    titleLabel->setProperty("role", "cardTitle");

    m_badge = new QLabel(this);
    m_badge->setProperty("role", "badge");
    m_badge->hide();

    header->addWidget(titleLabel);
    header->addStretch(1);
    header->addWidget(m_badge);
    outer->addLayout(header);

    m_body = new QVBoxLayout();
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(7);
    outer->addLayout(m_body);
}

void Card::setBadge(const QString& text) {
    m_badge->setText(text);
    m_badge->setVisible(!text.isEmpty());
}

void Card::setBadgeLive(bool live) {
    if (m_badge->property("live").toBool() == live) return;
    m_badge->setProperty("live", live);
    m_badge->style()->unpolish(m_badge);
    m_badge->style()->polish(m_badge);
    m_badge->update();
}

void Card::addWidget(QWidget* widget) { m_body->addWidget(widget); }

void Card::addSpacing(int pixels) { m_body->addSpacing(pixels); }

QLabel* Card::addValueRow(const QString& caption, const QString& value) {
    auto* row = new QWidget(this);
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto* key = new QLabel(caption, row);
    key->setProperty("role", "caption");
    auto* val = new QLabel(value, row);
    val->setProperty("role", "value");
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);

    lay->addWidget(key);
    lay->addStretch(1);
    lay->addWidget(val);
    m_body->addWidget(row);
    return val;
}

SidePanel::SidePanel(QWidget* parent) : QScrollArea(parent) {
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setAutoFillBackground(false);

    m_host = new QWidget(this);
    m_host->setAutoFillBackground(false);
    m_content = new QVBoxLayout(m_host);
    m_content->setContentsMargins(0, 0, 0, 0);
    m_content->setSpacing(10);
    setWidget(m_host);
}

Card* SidePanel::addCard(const QString& title) {
    auto* card = new Card(title, m_host);
    m_content->addWidget(card);
    return card;
}

void SidePanel::addStretch() { m_content->addStretch(1); }

}  // namespace ui
