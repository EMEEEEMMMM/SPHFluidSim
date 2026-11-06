#include "Ui/Theme.hpp"

#include <QCoreApplication>
#include <QSettings>

#include <algorithm>

namespace ui {
namespace {

QColor Rgba(int r, int g, int b, int a = 255) {
    QColor c(r, g, b);
    c.setAlpha(a);
    return c;
}
QColor White(int a) { return Rgba(255, 255, 255, a); }
QColor Black(int a) { return Rgba(0, 0, 0, a); }

// alpha-composite `top` over `bottom`. Widgets cannot rely on translucency the
// way QML can, so their style sheet uses the flattened colour instead.
QColor Flatten(const QColor& top, const QColor& bottom) {
    const qreal a = top.alphaF();
    return QColor::fromRgbF(top.redF() * a + bottom.redF() * (1.0 - a),
                            top.greenF() * a + bottom.greenF() * (1.0 - a),
                            top.blueF() * a + bottom.blueF() * (1.0 - a));
}

ThemeTokens MakeCupertinoDark() {
    ThemeTokens t;
    t.id = QStringLiteral("cupertino-dark");
    t.name = QStringLiteral("Cupertino Dark");
    t.blurb = QStringLiteral("Apple · Dark");
    t.dark = true;
    t.bg = Rgba(0x1B, 0x1B, 0x1D);
    t.chrome = Rgba(0x1F, 0x1F, 0x22, 242);
    t.panel = Rgba(0x1B, 0x1B, 0x1D);
    t.card = Rgba(0x25, 0x25, 0x28);
    t.field = Rgba(0x3A, 0x3A, 0x3D);
    t.border = White(19);
    t.borderStrong = White(42);
    t.fg = Rgba(0xF5, 0xF5, 0xF7);
    t.fg2 = Rgba(0xAE, 0xAE, 0xB4);
    t.fg3 = Rgba(0x86, 0x86, 0x8C);
    t.accent = Rgba(0x0A, 0x84, 0xFF);
    t.accentFg = Qt::white;
    t.accentSoft = Rgba(0x0A, 0x84, 0xFF, 40);
    t.segActive = Rgba(0x42, 0x42, 0x46);
    t.segActiveFg = Rgba(0xFF, 0xFF, 0xFF);
    t.switchOn = Rgba(0x30, 0xD1, 0x58);
    t.ok = Rgba(0x30, 0xD1, 0x58);
    t.warn = Rgba(0xFF, 0xD6, 0x0A);
    t.danger = Rgba(0xFF, 0x45, 0x3A);
    t.track = Rgba(0x46, 0x46, 0x4A);
    t.chip = Rgba(0x3A, 0x3A, 0x3D);
    t.sliderHandle = Rgba(0xFF, 0xFF, 0xFF);
    t.sliderHandleBorder = Black(52);
    t.shadowColor = Black(90);
    t.hud = Rgba(0x0A, 0x0B, 0x0E, 160);
    t.hudBorder = White(30);
    t.radiusLg = 12;
    t.radius = 10;
    t.radiusSm = 7;
    t.cardGap = 10;
    t.cardPad = 14;
    t.controlH = 30;
    t.trackH = 4;
    return t;
}

ThemeTokens MakeCupertinoLight() {
    ThemeTokens t;
    t.id = QStringLiteral("cupertino-light");
    t.name = QStringLiteral("Cupertino Light");
    t.blurb = QStringLiteral("Apple · Light");
    t.dark = false;
    t.bg = Rgba(0xF5, 0xF5, 0xF7);
    t.chrome = Rgba(0xFF, 0xFF, 0xFF, 246);
    t.panel = Rgba(0xF5, 0xF5, 0xF7);
    t.card = Rgba(0xFF, 0xFF, 0xFF);
    t.field = Rgba(0xEC, 0xEC, 0xEF);
    t.border = Rgba(0xE2, 0xE2, 0xE5);
    t.borderStrong = Rgba(0xCE, 0xCE, 0xD3);
    t.fg = Rgba(0x1D, 0x1D, 0x1F);
    t.fg2 = Rgba(0x63, 0x63, 0x68);
    t.fg3 = Rgba(0x86, 0x86, 0x8B);
    t.accent = Rgba(0x00, 0x7A, 0xFF);
    t.accentFg = Qt::white;
    t.accentSoft = Rgba(0x00, 0x7A, 0xFF, 26);
    t.segActive = Rgba(0x1D, 0x1D, 0x1F);
    t.segActiveFg = Qt::white;
    t.switchOn = Rgba(0x34, 0xC7, 0x59);
    t.ok = Rgba(0x24, 0x8A, 0x3D);
    t.warn = Rgba(0xB2, 0x50, 0x00);
    t.danger = Rgba(0xD7, 0x00, 0x15);
    t.track = Rgba(0xD7, 0xD7, 0xDC);
    t.chip = Rgba(0xF2, 0xF2, 0xF4);
    t.sliderHandle = Qt::white;
    t.sliderHandleBorder = Rgba(0x00, 0x00, 0x00, 42);
    t.shadowColor = Black(36);
    t.hud = Rgba(0x0C, 0x0E, 0x12, 150);
    t.hudBorder = White(28);
    t.radiusLg = 12;
    t.radius = 10;
    t.radiusSm = 7;
    t.cardGap = 10;
    t.cardPad = 14;
    t.controlH = 30;
    t.trackH = 4;
    return t;
}

ThemeTokens MakeLinearDark() {
    ThemeTokens t;
    t.id = QStringLiteral("linear-dark");
    t.name = QStringLiteral("Linear Dark");
    t.blurb = QStringLiteral("Developer · Dark");
    t.dark = true;
    t.bg = Rgba(0x08, 0x09, 0x0A);
    t.chrome = Rgba(0x0B, 0x0C, 0x0E);
    t.panel = Rgba(0x08, 0x09, 0x0A);
    t.card = Rgba(0x10, 0x11, 0x13);
    t.field = Rgba(0x17, 0x18, 0x1B);
    t.border = Rgba(0x22, 0x24, 0x29);
    t.borderStrong = Rgba(0x31, 0x34, 0x3B);
    t.fg = Rgba(0xEC, 0xED, 0xF1);
    t.fg2 = Rgba(0x9B, 0x9F, 0xA8);
    t.fg3 = Rgba(0x71, 0x76, 0x80);
    t.accent = Rgba(0x6E, 0x79, 0xD6);
    t.accentFg = Qt::white;
    t.accentSoft = Rgba(0x6E, 0x79, 0xD6, 38);
    t.segActive = Rgba(0x27, 0x29, 0x2E);
    t.segActiveFg = Rgba(0xF7, 0xF7, 0xFA);
    t.switchOn = Rgba(0x2B, 0xC9, 0x8A);
    t.ok = Rgba(0x2B, 0xC9, 0x8A);
    t.warn = Rgba(0xF2, 0xC9, 0x4C);
    t.danger = Rgba(0xEB, 0x57, 0x52);
    t.track = Rgba(0x25, 0x27, 0x2C);
    t.chip = Rgba(0x18, 0x19, 0x1D);
    t.sliderHandle = Rgba(0xEC, 0xED, 0xF1);
    t.sliderHandleBorder = Black(90);
    t.shadowColor = Black(120);
    t.hud = Rgba(0x0B, 0x0C, 0x0E, 168);
    t.hudBorder = White(24);
    t.radiusLg = 8;
    t.radius = 6;
    t.radiusSm = 4;
    t.backdrop = false;
    t.shadow = false;
    t.cardGap = 8;
    t.cardPad = 12;
    t.controlH = 26;
    t.trackH = 3;
    t.numberFamily = QStringLiteral("Consolas");
    return t;
}

ThemeTokens MakeSwissLight() {
    ThemeTokens t;
    t.id = QStringLiteral("swiss-light");
    t.name = QStringLiteral("Swiss Light");
    t.blurb = QStringLiteral("International · Light");
    t.dark = false;
    t.bg = Rgba(0xFF, 0xFF, 0xFF);
    t.chrome = Rgba(0xFF, 0xFF, 0xFF);
    t.panel = Rgba(0xFF, 0xFF, 0xFF);
    t.card = Rgba(0xFF, 0xFF, 0xFF);
    t.field = Rgba(0xF7, 0xF7, 0xF7);
    t.border = Rgba(0xDD, 0xDD, 0xDD);
    t.borderStrong = Rgba(0x11, 0x11, 0x11);
    t.fg = Rgba(0x0A, 0x0A, 0x0A);
    t.fg2 = Rgba(0x4F, 0x4F, 0x4F);
    t.fg3 = Rgba(0x6F, 0x6F, 0x6F);
    t.accent = Rgba(0xE8, 0x50, 0x1E);
    t.accentFg = Qt::white;
    t.accentSoft = Rgba(0xE8, 0x50, 0x1E, 20);
    t.segActive = Rgba(0x11, 0x11, 0x11);
    t.segActiveFg = Qt::white;
    t.switchOn = Rgba(0x1C, 0x7A, 0x3D);
    t.ok = Rgba(0x14, 0x7D, 0x3F);
    t.warn = Rgba(0xA3, 0x5A, 0x00);
    t.danger = Rgba(0xC0, 0x1B, 0x1B);
    t.track = Rgba(0xD8, 0xD8, 0xD8);
    t.chip = Rgba(0xF3, 0xF3, 0xF3);
    t.sliderHandle = Rgba(0x11, 0x11, 0x11);
    t.sliderHandleBorder = Rgba(0x11, 0x11, 0x11);
    t.shadowColor = Black(0);
    t.hud = Rgba(0x0C, 0x0E, 0x12, 150);
    t.hudBorder = White(24);
    t.radiusLg = 0;
    t.radius = 0;
    t.radiusSm = 0;
    t.square = true;
    t.pill = false;
    t.backdrop = false;
    t.shadow = false;
    t.cardGap = 10;
    t.cardPad = 12;
    t.controlH = 28;
    t.trackH = 4;
    t.numberFamily = QStringLiteral("Consolas");
    return t;
}

const ThemeTokens* ThemeTable() {
    static const ThemeTokens table[4] = {
        MakeCupertinoDark(), MakeCupertinoLight(), MakeLinearDark(), MakeSwissLight(),
    };
    return table;
}

QString Css(const QColor& c) {
    if (c.alpha() >= 255) return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(c.alpha());
}

}  // namespace

Theme& Theme::instance() {
    static Theme theme;
    return theme;
}

int Theme::count() { return 4; }

const ThemeTokens& Theme::tokens(int index) {
    const int i = (index < 0 || index >= 4) ? 0 : index;
    return ThemeTable()[i];
}

Theme::Theme() {
    loadSettings();
    if (QCoreApplication::instance()) {
        moveToThread(QCoreApplication::instance()->thread());
    }
}

Theme::~Theme() = default;

int Theme::index() const { return m_index; }

void Theme::setIndex(int index) {
    const int clamped = (index < 0 || index >= count()) ? 0 : index;
    if (clamped == m_index) return;
    m_index = clamped;
    saveSettings();
    Q_EMIT changed();
}

void Theme::cycle() { setIndex((m_index + 1) % count()); }

QString Theme::name() const { return tokens(m_index).name; }

bool Theme::isDark() const { return tokens(m_index).dark; }

const ThemeTokens& Theme::current() const { return tokens(m_index); }

QVariantMap Theme::colors() const {
    const ThemeTokens& t = current();
    QVariantMap m;
    m.insert(QStringLiteral("bg"), t.bg);
    m.insert(QStringLiteral("chrome"), t.chrome);
    m.insert(QStringLiteral("panel"), t.panel);
    m.insert(QStringLiteral("card"), t.card);
    m.insert(QStringLiteral("field"), t.field);
    m.insert(QStringLiteral("border"), t.border);
    m.insert(QStringLiteral("borderStrong"), t.borderStrong);
    m.insert(QStringLiteral("fg"), t.fg);
    m.insert(QStringLiteral("fg2"), t.fg2);
    m.insert(QStringLiteral("fg3"), t.fg3);
    m.insert(QStringLiteral("accent"), t.accent);
    m.insert(QStringLiteral("accentFg"), t.accentFg);
    m.insert(QStringLiteral("accentSoft"), t.accentSoft);
    m.insert(QStringLiteral("segActive"), t.segActive);
    m.insert(QStringLiteral("segActiveFg"), t.segActiveFg);
    m.insert(QStringLiteral("switchOn"), t.switchOn);
    m.insert(QStringLiteral("ok"), t.ok);
    m.insert(QStringLiteral("warn"), t.warn);
    m.insert(QStringLiteral("danger"), t.danger);
    m.insert(QStringLiteral("track"), t.track);
    m.insert(QStringLiteral("chip"), t.chip);
    m.insert(QStringLiteral("sliderHandle"), t.sliderHandle);
    m.insert(QStringLiteral("sliderHandleBorder"), t.sliderHandleBorder);
    m.insert(QStringLiteral("shadow"), t.shadowColor);
    m.insert(QStringLiteral("hud"), t.hud);
    m.insert(QStringLiteral("hudBorder"), t.hudBorder);
    return m;
}

QVariantList Theme::themes() const {
    QVariantList list;
    for (int i = 0; i < count(); ++i) {
        const ThemeTokens& t = tokens(i);
        QVariantMap item;
        item.insert(QStringLiteral("index"), i);
        item.insert(QStringLiteral("id"), t.id);
        item.insert(QStringLiteral("name"), t.name);
        item.insert(QStringLiteral("blurb"), t.blurb);
        item.insert(QStringLiteral("dark"), t.dark);
        list.append(item);
    }
    return list;
}

QVariantMap Theme::preview(int index) const {
    const ThemeTokens& t = tokens(index);
    QVariantMap m;
    m.insert(QStringLiteral("bg"), t.bg);
    m.insert(QStringLiteral("card"), t.card);
    m.insert(QStringLiteral("accent"), t.accent);
    m.insert(QStringLiteral("border"), t.border);
    m.insert(QStringLiteral("fg"), t.fg);
    m.insert(QStringLiteral("square"), t.square);
    return m;
}

QVariantMap Theme::metrics() const {
    const ThemeTokens& t = current();
    QVariantMap m;
    m.insert(QStringLiteral("radiusLg"), t.radiusLg);
    m.insert(QStringLiteral("radius"), t.radius);
    m.insert(QStringLiteral("radiusSm"), t.radiusSm);
    m.insert(QStringLiteral("square"), t.square);
    m.insert(QStringLiteral("pill"), t.pill);
    m.insert(QStringLiteral("backdrop"), t.backdrop);
    m.insert(QStringLiteral("shadow"), t.shadow);
    m.insert(QStringLiteral("titleCaps"), t.titleCaps);
    m.insert(QStringLiteral("baseFontPx"), t.baseFontPx);
    m.insert(QStringLiteral("cardGap"), t.cardGap);
    m.insert(QStringLiteral("cardPad"), t.cardPad);
    m.insert(QStringLiteral("controlH"), t.controlH);
    m.insert(QStringLiteral("trackH"), t.trackH);
    m.insert(QStringLiteral("numberFamily"), t.numberFamily);
    return m;
}

void Theme::loadSettings() {
    QSettings s;
    const int stored = s.value(QStringLiteral("ui/theme"), 0).toInt();
    m_index = (stored < 0 || stored >= count()) ? 0 : stored;
}

void Theme::saveSettings() const {
    QSettings s;
    s.setValue(QStringLiteral("ui/theme"), m_index);
}

QString Theme::styleSheet() const {
    const ThemeTokens& t = current();
    const QColor bgFlat = t.bg;
    const QColor chromeFlat = Flatten(t.chrome, bgFlat);
    const QColor panelFlat = Flatten(t.panel, bgFlat);
    const QColor cardFlat = Flatten(t.card, panelFlat);
    const QColor fieldFlat = Flatten(t.field, cardFlat);
    const QColor trackFlat = Flatten(t.track, cardFlat);
    const QColor chipFlat = Flatten(t.chip, cardFlat);
    const QColor borderFlat = Flatten(t.border, cardFlat);
    const QColor borderStrongFlat = Flatten(t.borderStrong, cardFlat);
    const QString fg = Css(t.fg);
    const QString fg2 = Css(t.fg2);
    const QString fg3 = Css(t.fg3);
    const QString cardSolid = Css(cardFlat);
    const QString sb = QStringLiteral(R"QSS(
QWidget { color: %1; font-size: %12px; }
#Root { background: %2; }
#TitleBar { background: %3; border-bottom: 1px solid %10; }
#StatusBar { background: %3; border-top: 1px solid %10; }
#SidePanel { background: %4; border: none; }
QFrame[card="true"] { background: %5; border: 1px solid %10; border-radius: %18px; }
QLabel { background: transparent; color: %1; }
QLabel[role="cardTitle"] { color: %7; font-size: 10px; font-weight: 600; }
QLabel[role="caption"] { color: %6; font-size: 11px; }
QLabel[role="value"] { color: %1; font-family: "%20"; font-size: 11px; }
QLabel[role="metricKey"] { color: %7; font-size: 10px; }
QLabel[role="metricValue"] { color: %1; font-family: "%20"; font-size: 17px; font-weight: 600; }
QLabel[role="brandName"] { color: %1; font-size: 13px; font-weight: 600; }
QLabel[role="brandSub"] { color: %7; font-size: 10px; }
QLabel[role="status"] { color: %7; font-size: 10px; }
QLabel[role="legend"] { color: %7; font-size: 10px; }
QLabel[role="badge"] { background: %13; border: 1px solid %10; border-radius: 9px; padding: 1px 7px; color: %6; font-size: 9px; }
QLabel[role="badge"][live="true"] { color: %30; border-color: %30; background: %31; }
#BrandMark { background: %9; border-radius: %19px; color: #FFFFFF; font-size: 11px; font-weight: 700; }
#Clock { background: %13; border: 1px solid %10; border-radius: 9px; padding: 3px 9px; color: %6; font-family: "%20"; font-size: 10px; }
QLabel[role="hud"] { color: #E8ECF4; font-size: 11px; }
QPushButton { background: %8; border: 1px solid %11; border-radius: %19px; padding: 5px 11px; color: %1; font-size: 11px; }
QPushButton:hover { background: %13; }
QPushButton:pressed { background: %14; }
QPushButton:disabled { color: %7; }
QPushButton[variant="primary"] { background: %9; color: #FFFFFF; border: none; font-weight: 600; }
QPushButton[variant="primary"]:hover { background: %15; }
QPushButton[variant="ghost"] { background: transparent; border: 1px solid %10; color: %6; }
QPushButton[variant="ghost"]:hover { background: %13; }
QPushButton[variant="icon"] { background: transparent; border: none; padding: 3px; }
QPushButton[variant="icon"]:hover { background: %13; border-radius: %19px; }
QPushButton[variant="play"] { background: %9; border: none; border-radius: 16px; }
QPushButton[variant="play"]:hover { background: %15; }
QToolButton { background: %8; border: 1px solid %10; border-radius: %19px; padding: 4px 8px; color: %1; font-size: 11px; }
QToolButton:hover { background: %13; border-color: %11; }
QToolButton:pressed { background: %14; }
QToolButton::menu-indicator { image: none; width: 0px; }
QMenuBar { background: %4; border-bottom: 1px solid %10; spacing: 2px; padding: 2px 8px; font-size: 11px; }
QMenuBar::item { background: transparent; color: %6; padding: 4px 9px; border-radius: %19px; }
QMenuBar::item:selected { background: %13; color: %1; }
QMenuBar::item:pressed { background: %14; color: %1; }
QMenu { background: %16; color: %1; border: 1px solid %10; border-radius: %19px; padding: 5px; }
QMenu::item { padding: 6px 22px 6px 10px; border-radius: %23px; }
QMenu::item:selected { background: %13; }
QMenu::item:checked { color: %9; }
QComboBox { background: %8; border: 1px solid %10; border-radius: %19px; padding: 4px 8px; color: %1; font-size: 11px; min-width: 118px; }
QComboBox:hover { border-color: %11; }
QComboBox::drop-down { border: none; width: 16px; }
QComboBox QAbstractItemView { background: %16; color: %1; border: 1px solid %10; selection-background-color: %9; outline: none; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 9px; margin: 2px 0 2px 0; }
QScrollBar::handle:vertical { background: %17; border-radius: 4px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: %11; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QSlider { min-height: 18px; background: transparent; }
QSlider::groove:horizontal { height: %21px; background: %14; border-radius: %22px; }
QSlider::sub-page:horizontal { height: %21px; background: %9; border-radius: %22px; }
QSlider::handle:horizontal { width: %24px; height: %25px; margin: -%26px 0; background: %27; border: 1px solid %28; border-radius: %29px; }
QSlider::handle:horizontal:hover { border-color: %9; }
QSlider::handle:horizontal:disabled { background: %13; }
QToolTip { background: %16; color: %1; border: 1px solid %10; padding: 4px 6px; }
QDialog { background: %5; color: %1; }
QGroupBox { background: transparent; border: 1px solid %10; border-radius: %18px; margin-top: 14px; padding: 10px 10px 8px 10px; color: %7; font-size: 10px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; color: %7; }
QLineEdit, QSpinBox, QDoubleSpinBox { background: %8; border: 1px solid %10; border-radius: %19px; padding: 4px 7px; color: %1; font-family: "%20"; font-size: 11px; selection-background-color: %9; selection-color: #FFFFFF; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border: 2px solid %9; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: %7; }
QCheckBox { color: %6; spacing: 6px; font-size: 11px; }
QProgressBar { background: %14; border: none; border-radius: 4px; min-height: 8px; max-height: 8px; text-align: center; color: transparent; }
QProgressBar::chunk { background: %9; border-radius: 4px; }
QTabWidget::pane { border: 1px solid %10; border-radius: %19px; background: %5; }
QTabBar::tab { background: transparent; color: %7; padding: 6px 12px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: %1; border-bottom-color: %9; }
)QSS");
    const QString numericFont =
        t.numberFamily.isEmpty() ? QStringLiteral("Segoe UI") : t.numberFamily;
    const int trackPx = std::max(2, static_cast<int>(t.trackH));
    const int handleW = t.square ? 8 : 14;
    const int handleH = t.square ? 16 : 14;
    return sb.arg(fg)                       // 1
        .arg(Css(bgFlat))                   // 2
        .arg(Css(chromeFlat))               // 3
        .arg(Css(panelFlat))                // 4
        .arg(Css(cardFlat))                 // 5
        .arg(fg2)                           // 6
        .arg(fg3)                           // 7
        .arg(Css(fieldFlat))                // 8
        .arg(Css(t.accent))                 // 9
        .arg(Css(borderFlat))               // 10
        .arg(Css(borderStrongFlat))         // 11
        .arg(t.baseFontPx)                  // 12
        .arg(Css(chipFlat))                 // 13
        .arg(Css(trackFlat))                // 14
        .arg(Css(t.accent.lighter(112)))    // 15
        .arg(cardSolid)                     // 16
        .arg(Css(trackFlat))                // 17
        .arg(static_cast<int>(t.radiusLg))  // 18
        .arg(static_cast<int>(t.radiusSm))  // 19
        .arg(numericFont)                   // 20
        .arg(trackPx)                       // 21
        .arg(trackPx / 2)                   // 22
        .arg(static_cast<int>(t.radiusSm))  // 23
        .arg(handleW)                       // 24
        .arg(handleH)                       // 25
        .arg(6)                             // 26
        .arg(Css(t.sliderHandle))           // 27
        .arg(Css(t.sliderHandleBorder))     // 28
        .arg(t.square ? 0 : handleH / 2)    // 29
        .arg(Css(t.switchOn))               // 30
        .arg(Css(t.accentSoft));            // 31
}

}  // namespace ui
