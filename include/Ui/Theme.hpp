// Theme — one token set, four skins.
//
// The four themes are the preview boards the user picked from (Cupertino Dark /
// Cupertino Light / Linear Dark / Swiss Light), reduced to role-based tokens.
// Accent stays system blue in all four: it is the product's interactive colour,
// not part of the skin.
//
// Both front-ends read the same tokens: the widget front-end compiles them into
// a Qt style sheet, the QML front-end binds to them through the `Theme`
// singleton.
#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace ui {

struct ThemeTokens {
    QString id;
    QString name;
    QString blurb;
    bool dark = true;

    // surfaces
    QColor bg;
    QColor chrome;
    QColor panel;
    QColor card;
    QColor field;
    QColor border;
    QColor borderStrong;

    // text
    QColor fg;
    QColor fg2;
    QColor fg3;

    // interaction
    QColor accent;
    QColor accentFg;
    QColor accentSoft;
    QColor segActive;
    QColor segActiveFg;
    QColor switchOn;
    QColor ok;
    QColor warn;
    QColor danger;
    QColor track;
    QColor chip;
    QColor sliderHandle;
    QColor sliderHandleBorder;
    QColor shadowColor;

    // viewport overlay (always dark: the render target is a dark 3D scene)
    QColor hud;
    QColor hudBorder;

    // geometry / typography
    qreal radiusLg = 14.0;
    qreal radius = 10.0;
    qreal radiusSm = 7.0;
    bool square = false;        // Swiss: no rounding anywhere
    bool pill = true;           // round play button
    bool backdrop = true;       // frosted side panels
    bool shadow = true;         // card drop shadow
    bool titleCaps = true;      // section labels in caps
    int baseFontPx = 13;
    int cardGap = 10;
    int cardPad = 12;
    int controlH = 28;
    qreal trackH = 4.0;
    QString numberFamily;       // empty = UI font; Swiss/Linear use tabular digits
};

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(int index READ index WRITE setIndex NOTIFY changed)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(QVariantMap metrics READ metrics NOTIFY changed)
    Q_PROPERTY(QVariantList themes READ themes NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(bool dark READ isDark NOTIFY changed)

public:
    static Theme& instance();
    static int count();
    static const ThemeTokens& tokens(int index);

    ~Theme() override;

    int index() const;
    Q_INVOKABLE void setIndex(int index);
    Q_INVOKABLE void cycle();
    QString name() const;
    bool isDark() const;

    QVariantMap colors() const;
    QVariantMap metrics() const;
    QVariantList themes() const;
    Q_INVOKABLE QVariantMap preview(int index) const;
    const ThemeTokens& current() const;

    // Qt style sheet for the widget front-end.
    QString styleSheet() const;

    void loadSettings();
    void saveSettings() const;

Q_SIGNALS:
    void changed();

private:
    Theme();

    int m_index = 0;
};

}  // namespace ui
