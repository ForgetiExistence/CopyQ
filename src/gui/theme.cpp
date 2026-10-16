// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme.h"

#include "ui_configtabappearance.h"

#include "common/config.h"
#include "common/log.h"
#include "gui/iconfont.h"
#include "platform/platformnativeinterface.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QListView>
#include <QScreen>
#include <QSettings>
#include <QStyleFactory>

#if QT_VERSION >= QT_VERSION_CHECK(6,5,0)
#   include <QStyleHints>
#endif

#include <cmath>

namespace {

/// Up to this value of background lightness, window title style would be light.
const int lightThreshold = 100;

} // namespace

/// Windows 11 / Fluent 2 typography: the Segoe UI Variable type ramp. The UI
/// font is the "Text" optical instance; the editor uses Cascadia Mono, the
/// Windows terminal/console font, at the same 10pt body size.
const QString win11UiFont("Segoe UI Variable Text");
const QString win11MonoFont("Cascadia Mono");

/// Build a QFont::toString() descriptor for the given family and size (pt).
/// The fixed field layout (family, pointSize, pixelSize, styleHint, weight,
/// then the style flags) is the format QFont::fromString accepts.
QString win11FontString(const QString &family, double pointSize)
{
    return QStringLiteral("%1,%2,-1,5,50,0,0,0,0,0").arg(family).arg(pointSize);
}

const QLatin1String defaultColorVarBase("default_bg");
const QLatin1String defaultColorVarText("default_text");
const QLatin1String defaultColorVarPlaceholderText("default_placeholder_text");
const QLatin1String defaultColorVarAlternateBase("default_alt_bg");
const QLatin1String defaultColorVarHighlight("default_highlight_bg");
const QLatin1String defaultColorVarHighlightText("default_highlight_text");
const QLatin1String defaultColorVarToolTipBase("default_tooltip_bg");
const QLatin1String defaultColorVarToolTipText("default_tooltip_text");
const QLatin1String defaultColorVarWindow("default_window");
const QLatin1String defaultColorVarWindowText("default_window_text");
const QLatin1String defaultColorVarButton("default_button");
const QLatin1String defaultColorVarButtonText("default_button_text");
const QLatin1String defaultColorVarBrightText("default_bright_text");
const QLatin1String defaultColorVarLight("default_light");
const QLatin1String defaultColorVarMidlight("default_midlight");
const QLatin1String defaultColorVarDark("default_dark");
const QLatin1String defaultColorVarMid("default_mid");
const QLatin1String defaultColorVarShadow("default_shadow");
const QLatin1String defaultColorLink("default_link");
const QLatin1String defaultColorLinkVisited("default_link_visited");

QPalette::ColorRole defaultColorVarToRole(const QString &varName)
{
    static QHash<QString, QPalette::ColorRole> map = {
        {defaultColorVarBase, QPalette::Base},
        {defaultColorVarText, QPalette::Text},
        {defaultColorVarPlaceholderText, QPalette::PlaceholderText},
        {defaultColorVarAlternateBase, QPalette::AlternateBase},
        {defaultColorVarHighlight, QPalette::Highlight},
        {defaultColorVarHighlightText, QPalette::HighlightedText},
        {defaultColorVarToolTipBase, QPalette::ToolTipBase},
        {defaultColorVarToolTipText, QPalette::ToolTipText},
        {defaultColorVarWindow, QPalette::Window},
        {defaultColorVarWindowText, QPalette::WindowText},
        {defaultColorVarButton, QPalette::Button},
        {defaultColorVarButtonText, QPalette::ButtonText},
        {defaultColorVarBrightText, QPalette::BrightText},
        {defaultColorVarLight, QPalette::Light},
        {defaultColorVarMidlight, QPalette::Midlight},
        {defaultColorVarDark, QPalette::Dark},
        {defaultColorVarMid, QPalette::Mid},
        {defaultColorVarShadow, QPalette::Shadow},
        {defaultColorLink, QPalette::Link},
        {defaultColorLinkVisited, QPalette::LinkVisited},
    };
    const auto it = map.find(varName);
    return (it == std::end(map)) ? QPalette::NoRole : it.value();
}

double normalizeFactor(double value)
{
    return qBound( 0.0, value, 1.0 );
}

int normalizeColorValue(int value)
{
    return qBound(0, value, 255);
}

/// Add RGB components properly.
int addColor(int c1, float multiply, int c2)
{
    const float f1 = static_cast<float>(c1);
    const float f2 = static_cast<float>(c2);
    return multiply > 0.0f
            ? static_cast<int>( std::sqrt(f1*f1 + multiply * f2*f2) )
            : c1 + static_cast<int>(multiply * f2);
}

void addColor(
        const QString &color, float multiply, int *r, int *g, int *b, int *a,
        const Theme &theme, const Values &values, int maxRecursion)
{
    if (color.isEmpty())
        return;

    QColor toAdd;
    float x = multiply;

    if (color.at(0).isDigit()) {
        bool ok;
        x = multiply * color.toFloat(&ok);
        if (!ok)
            return;
        toAdd = QColor(1, 1, 1);
    } else if ( color.startsWith('#') || color.startsWith("rgba(") ) {
        toAdd = deserializeColor(color);
        if ( !toAdd.isValid() )
            toAdd = QColor(Qt::black);
    } else if ( color.startsWith(QStringLiteral("default_")) ) {
        const QPalette::ColorRole role = defaultColorVarToRole(color);
        toAdd = QPalette().color(role);
    } else if (QColor col(color); col.isValid()) {
        toAdd = col;
    } else if (maxRecursion > 0) {
        const QVariant v = values.contains(color)
                ? values.value(color) : theme.value(color);
        toAdd = evalColor(v.toString(), theme, values, maxRecursion - 1);
    }

    *r = normalizeColorValue( addColor(*r, x, toAdd.red()) );
    *g = normalizeColorValue( addColor(*g, x, toAdd.green()) );
    *b = normalizeColorValue( addColor(*b, x, toAdd.blue()) );
    if (multiply > 0.0f) {
        const float fa = static_cast<float>(*a);
        const float fad = static_cast<float>(toAdd.alpha());
        *a = normalizeColorValue( static_cast<int>(fa + x * fad) );
    }
}

int fitFontWeight(int weight, int low, int high, int highCss)
{
    const int diffCss = 100;
    const int lowCss = highCss - diffCss;
    if (weight == low)
        return lowCss;

    const int diff = high - low;
    return lowCss + diffCss * weight / diff;
}

int getFontWeightForStyleSheet(int weight)
{
    if (weight < QFont::ExtraLight)
        return fitFontWeight(weight, QFont::Thin, QFont::ExtraLight, 200);
    if (weight < QFont::Light)
        return fitFontWeight(weight, QFont::ExtraLight, QFont::Light, 300);
    if (weight < QFont::Normal)
        return fitFontWeight(weight, QFont::Light, QFont::Normal, 400);
    if (weight < QFont::Medium)
        return fitFontWeight(weight, QFont::Normal, QFont::Medium, 500);
    if (weight < QFont::DemiBold)
        return fitFontWeight(weight, QFont::Medium, QFont::DemiBold, 600);
    if (weight < QFont::Bold)
        return fitFontWeight(weight, QFont::DemiBold, QFont::Bold, 700);
    if (weight < QFont::ExtraBold)
        return fitFontWeight(weight, QFont::Bold, QFont::ExtraBold, 800);
    if (weight < QFont::Black)
        return fitFontWeight(weight, QFont::ExtraBold, QFont::Black, 900);

    return 900;
}

QString getFontStyleSheet(const QString &fontString, double scale = 1.0)
{
    QFont font;
    if (!fontString.isEmpty())
        font.fromString(fontString);

    qreal size = font.pointSizeF() * scale;
    QString sizeUnits = "pt";
    if (size < 0.0) {
        size = font.pixelSize() * scale;
        sizeUnits = "px";
    }
    size = qBound(0.1, size, 50.0);

    QString result;
    result.append( QString::fromLatin1(";font-family: \"%1\"").arg(font.family()) );
    result.append( QString::fromLatin1(";font:%1 %2 %3%4")
                   .arg(font.style() == QFont::StyleItalic
                        ? "italic" : font.style() == QFont::StyleOblique ? "oblique" : "normal",
                        font.bold() ? "bold" : "")
                   .arg(size)
                   .arg(sizeUnits)
                   );
    result.append( QString::fromLatin1(";text-decoration:%1 %2 %3")
                   .arg(font.strikeOut() ? "line-through" : "",
                        font.underline() ? "underline" : "",
                        font.overline() ? "overline" : "")
                   );

    const int w = getFontWeightForStyleSheet( font.weight() );
    result.append( QString::fromLatin1(";font-weight:%1").arg(w) );

    result.append(";");

    return result;
}

QString themePrefix()
{
#ifdef COPYQ_THEME_PREFIX
    return adjustedInstallPath(QStringLiteral(COPYQ_THEME_PREFIX));
#else
    return platformNativeInterface()->themePrefix();
#endif
}

double getFactor(const QString &name, const QHash<QString, QVariant> &values)
{
    bool ok;
    const double scale = values.value(name).toDouble(&ok);
    return ok ? scale : 1.0;
}

Theme::Theme(const QSettings &settings)
{
    loadTheme(settings);
}

Theme::Theme(Ui::ConfigTabAppearance *ui)
    : ui(ui)
{
}

void Theme::loadTheme(const QSettings &settings)
{
    resetTheme();

    for ( auto key = m_theme.keyBegin(); key != m_theme.keyEnd(); ++key ) {
        const auto value = settings.value(*key);
        if ( value.isValid() )
            m_theme[*key].setValue(value);
    }

    updateTheme();
}

void Theme::saveTheme(QSettings *settings) const
{
    QStringList keys = m_theme.keys();
    keys.sort();

    for (const auto &key : keys)
        settings->setValue( key, value(key) );
}

QVariant Theme::value(const QString &name) const
{
    return m_theme.value(name).value();
}

QColor Theme::color(const QString &name) const
{
    return evalColor( value(name).toString(), *this );
}

QFont Theme::font(const QString &name) const
{
    return themeFontFromString( value(name).toString() );
}

QColor Theme::evalColorExpression(const QString &expr) const
{
    return evalColor( expr, *this );
}

void Theme::decorateBrowser(QListView *c) const
{
    QAbstractScrollArea *c2 = c;
    decorateBrowser(c2);

    bool ok;
    const int itemSpacing = value("item_spacing").toInt(&ok);
    c->setSpacing( ok ? itemSpacing : c->fontMetrics().lineSpacing() / 6 );
}

void Theme::decorateMainWindow(QWidget *mainWindow) const
{
    QPalette palette = QApplication::palette();

    // This seems to properly reset icon colors.
    mainWindow->setStyleSheet(QString());
    mainWindow->setPalette(palette);

    const auto bg = color("bg");

#if QT_VERSION >= QT_VERSION_CHECK(6,8,0)
    // Change window dark/light title scheme if supported.
    QStyleHints *styleHints = QGuiApplication::styleHints();
    if ( !isMainWindowThemeEnabled() )
        styleHints->unsetColorScheme();
    else if (bg.lightness() < lightThreshold)
        styleHints->setColorScheme(Qt::ColorScheme::Dark);
    else
        styleHints->setColorScheme(Qt::ColorScheme::Light);
#endif

    if ( !isMainWindowThemeEnabled() ) {
        const QString cssTemplate = QStringLiteral("main_window_simple");
        mainWindow->setStyleSheet(getStyleSheet(cssTemplate));
        return;
    }

    const auto fg = color("fg");
    palette.setColor( QPalette::Base, bg );
    palette.setColor( QPalette::AlternateBase, color("alt_bg") );
    palette.setColor( QPalette::Text, fg );
    palette.setColor( QPalette::Window, bg );
    palette.setColor( QPalette::WindowText, fg );
    palette.setColor( QPalette::Button, color("button_bg") );
    palette.setColor( QPalette::ButtonText, color("button_fg") );
    palette.setColor( QPalette::Highlight, color("sel_bg") );
    palette.setColor( QPalette::HighlightedText, color("sel_fg") );
    palette.setColor( QPalette::Link, color("link") );
    palette.setColor( QPalette::LinkVisited, color("link_visited") );

    mainWindow->setPalette(palette);
    const QString cssTemplate = value("css_template_main_window").toString();
    mainWindow->setStyleSheet(getStyleSheet(cssTemplate));
}

void Theme::applyApplicationPalette() const
{
    if ( !isMainWindowThemeEnabled() )
        return;

    QPalette palette = QApplication::palette();
    const auto bg = color("bg");
    const auto fg = color("fg");
    palette.setColor( QPalette::Base, bg );
    palette.setColor( QPalette::AlternateBase, color("alt_bg") );
    palette.setColor( QPalette::Text, fg );
    palette.setColor( QPalette::Window, bg );
    palette.setColor( QPalette::WindowText, fg );
    palette.setColor( QPalette::Button, color("button_bg") );
    palette.setColor( QPalette::ButtonText, color("button_fg") );
    palette.setColor( QPalette::ToolTipBase, color("notes_bg") );
    palette.setColor( QPalette::ToolTipText, color("notes_fg") );
    palette.setColor( QPalette::PlaceholderText, color("num_fg") );
    palette.setColor( QPalette::Highlight, color("sel_bg") );
    palette.setColor( QPalette::HighlightedText, color("sel_fg") );
    palette.setColor( QPalette::Link, color("link") );
    palette.setColor( QPalette::LinkVisited, color("link_visited") );
    palette.setColor( QPalette::Light, color("light") );
    palette.setColor( QPalette::Midlight, color("midlight") );
    palette.setColor( QPalette::Dark, color("dark") );
    palette.setColor( QPalette::Mid, color("mid") );
    palette.setColor( QPalette::Shadow, color("shadow") );
    QApplication::setPalette(palette);

    const QFont appFont = font("font");
    if ( !appFont.family().isEmpty() )
        QApplication::setFont(appFont);
}

void Theme::applyWin11PaletteDefaults()
{
    // Seed the Qt default palette with the Windows 11 surface ramp so that the
    // default_* color variables (resolved through QPalette in
    // defaultColorVarToRole / deserializeColor) return Win11 values even before
    // a theme with style_main_window=true is loaded. Call once at startup,
    // before any Theme is constructed.
    //
    // The ramp must follow the OS light/dark preference: the default_* variables
    // are the fallback for every theme that does not override them, so seeding
    // the light ramp unconditionally would force the whole UI light even on a
    // dark Windows setup.
    const bool dark = isSystemDarkTheme();
    QPalette palette = QApplication::palette();

    if (dark) {
        palette.setColor( QPalette::Base, QColor(0x20, 0x20, 0x20) );
        palette.setColor( QPalette::AlternateBase, QColor(0x2B, 0x2B, 0x2B) );
        palette.setColor( QPalette::Text, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::Window, QColor(0x20, 0x20, 0x20) );
        palette.setColor( QPalette::WindowText, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::Button, QColor(0x33, 0x33, 0x33) );
        palette.setColor( QPalette::ButtonText, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::Highlight, QColor(0x4C, 0xC2, 0xFF) );
        palette.setColor( QPalette::HighlightedText, QColor(0x00, 0x00, 0x00) );
        palette.setColor( QPalette::ToolTipBase, QColor(0x2B, 0x2B, 0x2B) );
        palette.setColor( QPalette::ToolTipText, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::PlaceholderText, QColor(0x9A, 0x9A, 0x9A) );
        palette.setColor( QPalette::BrightText, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::Light, QColor(0x3A, 0x3A, 0x3A) );
        palette.setColor( QPalette::Midlight, QColor(0x2D, 0x2D, 0x2D) );
        palette.setColor( QPalette::Dark, QColor(0x18, 0x18, 0x18) );
        palette.setColor( QPalette::Mid, QColor(0x52, 0x52, 0x52) );
        palette.setColor( QPalette::Shadow, QColor(0x00, 0x00, 0x00) );
        palette.setColor( QPalette::Link, QColor(0x4C, 0xC2, 0xFF) );
        palette.setColor( QPalette::LinkVisited, QColor(0x4C, 0xC2, 0xFF) );
    } else {
        palette.setColor( QPalette::Base, QColor(0xF3, 0xF3, 0xF3) );
        palette.setColor( QPalette::AlternateBase, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::Text, QColor(0x1A, 0x1A, 0x1A) );
        palette.setColor( QPalette::Window, QColor(0xF3, 0xF3, 0xF3) );
        palette.setColor( QPalette::WindowText, QColor(0x1A, 0x1A, 0x1A) );
        palette.setColor( QPalette::Button, QColor(0xFB, 0xFB, 0xFB) );
        palette.setColor( QPalette::ButtonText, QColor(0x1A, 0x1A, 0x1A) );
        // AccentFillColorDefault (light) = SystemAccentColorDark1. #0078D4 is
        // the bare SystemAccentColor, reserved for AccentFillColorSelectedTextBackground.
        palette.setColor( QPalette::Highlight, QColor(0x00, 0x67, 0xC0) );
        palette.setColor( QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::ToolTipBase, QColor(0xF9, 0xF9, 0xF9) );
        palette.setColor( QPalette::ToolTipText, QColor(0x1A, 0x1A, 0x1A) );
        palette.setColor( QPalette::PlaceholderText, QColor(0x6E, 0x6E, 0x6E) );
        palette.setColor( QPalette::BrightText, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::Light, QColor(0xFF, 0xFF, 0xFF) );
        palette.setColor( QPalette::Midlight, QColor(0xED, 0xED, 0xED) );
        palette.setColor( QPalette::Dark, QColor(0xA0, 0xA0, 0xA0) );
        palette.setColor( QPalette::Mid, QColor(0xD1, 0xD1, 0xD1) );
        palette.setColor( QPalette::Shadow, QColor(0x80, 0x80, 0x80) );
        palette.setColor( QPalette::Link, QColor(0x00, 0x67, 0xC0) );
        palette.setColor( QPalette::LinkVisited, QColor(0x00, 0x67, 0xC0) );
    }

    QApplication::setPalette(palette);
}

void Theme::decorateScrollArea(QAbstractScrollArea *scrollArea) const
{
    const auto scrollbarPolicy = this->scrollbarPolicy();
    scrollArea->setVerticalScrollBarPolicy(scrollbarPolicy);
    scrollArea->setHorizontalScrollBarPolicy(scrollbarPolicy);
}

void Theme::decorateItemPreview(QAbstractScrollArea *itemPreview) const
{
    decorateBrowser(itemPreview);
}

QString Theme::getMenuStyleSheet() const
{
    const QString cssTemplate = value("css_template_menu").toString();
    return getStyleSheet(cssTemplate);
}

QString Theme::getNotificationStyleSheet() const
{
    const QString cssTemplate = value("css_template_notification").toString();
    return getStyleSheet(cssTemplate);
}

Qt::ScrollBarPolicy Theme::scrollbarPolicy() const
{
    return value("show_scrollbars").toBool()
            ? Qt::ScrollBarAsNeeded
            : Qt::ScrollBarAlwaysOff;
}

bool Theme::useSystemIcons() const
{
    return value("use_system_icons").toBool();
}

QFont Theme::themeFontFromString(const QString &fontString) const
{
    QFont font;
    if ( !fontString.isEmpty() )
        font.fromString(fontString);
    if ( !isAntialiasingEnabled() )
        font.setStyleStrategy(QFont::NoAntialias);
    return font;
}

bool Theme::isAntialiasingEnabled() const
{
    return m_antialiasing;
}

bool Theme::isSystemDarkTheme()
{
#if QT_VERSION >= QT_VERSION_CHECK(6,5,0)
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark)
        return true;
    if (scheme == Qt::ColorScheme::Light)
        return false;
#endif

#ifdef Q_OS_WIN
    // Fallback for Qt < 6.5 where QStyleHints::colorScheme() is unavailable
    // and for the "no preference" case.
    QSettings personalize(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return personalize.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
#else
    return false;
#endif
}

void Theme::resetTheme()
{
    m_theme["bg"]        = Option(defaultColorVarBase, "VALUE", ui ? ui->pushButtonColorBg : nullptr);
    m_theme["edit_bg"]   = Option(defaultColorVarBase, "VALUE", ui ? ui->pushButtonColorEditorBg : nullptr);
    m_theme["fg"]        = Option(defaultColorVarText, "VALUE", ui ? ui->pushButtonColorFg : nullptr);
    m_theme["edit_fg"]   = Option(defaultColorVarText, "VALUE", ui ? ui->pushButtonColorEditorFg : nullptr);
    m_theme["num_fg"]    = Option(defaultColorVarPlaceholderText, "VALUE", ui ? ui->pushButtonColorNumberFg : nullptr);
    m_theme["alt_bg"]    = Option(defaultColorVarAlternateBase, "VALUE", ui ? ui->pushButtonColorAltBg : nullptr);
    m_theme["sel_bg"]    = Option(defaultColorVarHighlight, "VALUE", ui ? ui->pushButtonColorSelBg : nullptr);
    m_theme["sel_fg"]    = Option(defaultColorVarHighlightText, "VALUE", ui ? ui->pushButtonColorSelFg : nullptr);
    m_theme["find_bg"]   = Option("yellow", "VALUE", ui ? ui->pushButtonColorFoundBg : nullptr);
    m_theme["find_fg"]   = Option("black", "VALUE", ui ? ui->pushButtonColorFoundFg : nullptr);
    m_theme["notes_bg"]  = Option(defaultColorVarToolTipBase, "VALUE", ui ? ui->pushButtonColorNotesBg : nullptr);
    m_theme["notes_fg"]  = Option(defaultColorVarToolTipText, "VALUE", ui ? ui->pushButtonColorNotesFg : nullptr);
    m_theme["notification_bg"]  = Option("#333", "VALUE", ui ? ui->pushButtonColorNotificationBg : nullptr);
    m_theme["notification_fg"]  = Option("#ddd", "VALUE", ui ? ui->pushButtonColorNotificationFg : nullptr);

    // Win11 surface ramp used by the application palette (see applyApplicationPalette).
    // Button surfaces follow WinUI ControlFillColorDefault/Secondary; the link
    // accent follows AccentTextFillColorPrimaryBrush.
    //
    // These resolve through QPalette (see applyWin11PaletteDefaults), so they
    // follow the OS light/dark preference instead of forcing light. Themes can
    // still override any of them explicitly.
    m_theme["button_bg"]       = Option(defaultColorVarButton);
    m_theme["button_fg"]       = Option(defaultColorVarButtonText);
    m_theme["link"]            = Option(defaultColorLink);
    m_theme["link_visited"]    = Option(defaultColorLinkVisited);
    m_theme["light"]           = Option(defaultColorVarLight);
    m_theme["midlight"]        = Option(defaultColorVarMidlight);
    m_theme["mid"]             = Option(defaultColorVarMid);
    m_theme["dark"]            = Option(defaultColorVarDark);
    m_theme["shadow"]          = Option(defaultColorVarShadow);

    // Win11 layered surfaces. Fluent 2 stacks three planes and separates them
    // with a 1px stroke rather than by a colour change alone:
    //   window (bg)    -> chrome plane: menu bar, tool bar, tab strip
    //   card (card_bg) -> content plane: the clipboard list sits on this
    //   row            -> transparent at rest, so the card shows through
    // Without a distinct card plane every region shares one flat colour and the
    // window reads as an undivided slab. See win11_items.css / main_window.css.
    // They default to the chrome colour so that themes which do not opt in
    // (nord, onedark, ...) keep their previous flat look. win11_light.ini and
    // win11_dark.ini override each one with the real WinUI layer value.
    //
    // The defaults reference the palette (default_*), not ${...}: a bare
    // "${bg}" is not a colour expression the parser understands, so it would
    // resolve to transparent instead of to the theme background.
    m_theme["card_bg"]         = Option(defaultColorVarBase);
    m_theme["card_stroke"]     = Option(defaultColorVarMidlight);
    m_theme["divider"]         = Option(defaultColorVarMidlight);
    m_theme["subtle_hover"]    = Option(defaultColorVarMidlight);
    m_theme["subtle_pressed"]  = Option(defaultColorVarMid);
    m_theme["control_stroke"]  = Option(defaultColorVarMid);

    // Colour of the window plane behind the widgets. Themes set this to
    // "transparent" when the Mica backdrop is on, so the compositor shows
    // through; everything layered on top (cards, chrome, menus) keeps its own
    // opaque surface.
    m_theme["window_bg"]       = Option(defaultColorVarBase);

    m_theme["font"]        = Option(win11FontString(win11UiFont, 10), "VALUE", ui ? ui->pushButtonFont : nullptr);
    m_theme["edit_font"]   = Option(win11FontString(win11MonoFont, 10), "VALUE", ui ? ui->pushButtonEditorFont : nullptr);
    m_theme["find_font"]   = Option(win11FontString(win11UiFont, 10), "VALUE", ui ? ui->pushButtonFoundFont : nullptr);
    m_theme["num_font"]    = Option(win11FontString(win11UiFont, 8), "VALUE", ui ? ui->pushButtonNumberFont : nullptr);
    m_theme["notes_font"]  = Option(win11FontString(win11UiFont, 10), "VALUE", ui ? ui->pushButtonNotesFont : nullptr);
    m_theme["notification_font"]  = Option(win11FontString(win11UiFont, 10), "VALUE", ui ? ui->pushButtonNotificationFont : nullptr);
    m_theme["show_number"] = Option(true, "checked", ui ? ui->checkBoxShowNumber : nullptr);
    m_theme["show_scrollbars"] = Option(true, "checked", ui ? ui->checkBoxScrollbars : nullptr);

    m_theme["css"] = Option("");
    m_theme["menu_css"] = Option(
                "\n    ;border: 1px solid ${midlight}"
                "\n    ;background: ${button_bg}"
                "\n    ;color: ${fg}"
                "\n    ;border-radius: 8px"
                "\n    ;padding: 4px"
                );
    m_theme["menu_bar_css"] = Option(
                "\n    ;background: ${bg}"
                "\n    ;color: ${fg}"
                "\n    ;border-bottom: 1px solid ${divider}"
                );
    m_theme["menu_bar_selected_css"] = Option(
                "\n    ;background: ${midlight}"
                "\n    ;color: ${fg}"
                "\n    ;border-radius: 4px"
                );
    m_theme["menu_bar_disabled_css"] = Option(
                "\n    ;color: ${num_fg}"
                );

    m_theme["item_css"] = Option(
                "\n    ;padding: 0.4em 0.6em"
                );
    m_theme["alt_item_css"] = Option("");
    m_theme["sel_item_css"] = Option(
                "\n    ;border-radius: 4px"
                );
    m_theme["hover_item_css"] = Option(
                "\n    ;border-radius: 4px"
                );
    m_theme["cur_item_css"] = Option(
                "\n    ;border: 2px solid ${sel_bg}"
                "\n    ;border-radius: 4px"
                );
    m_theme["item_spacing"] = Option(2);
    m_theme["notes_css"] = Option("");

    m_theme["tab_bar_css"] = Option(
                "\n    ;background: ${bg}"
                "\n    ;border-bottom: 1px solid ${divider}"
                );
    m_theme["tab_bar_tab_selected_css"] = Option(
                "\n    ;padding: 0.5em 0.9em"
                "\n    ;background: ${bg}"
                "\n    ;border: 0"
                "\n    ;border-bottom: 2px solid ${sel_bg}"
                "\n    ;color: ${fg}"
                );
    m_theme["tab_bar_tab_unselected_css"] = Option(
                "\n    ;padding: 0.5em 0.9em"
                "\n    ;background: ${bg}"
                "\n    ;border: 0"
                "\n    ;border-bottom: 2px solid transparent"
                "\n    ;color: ${num_fg}"
                );
    m_theme["tab_bar_scroll_buttons_css"] = Option(
                "\n    ;background: transparent"
                "\n    ;color: ${fg}"
                "\n    ;border: 0"
                );
    m_theme["tab_bar_item_counter"] = Option(
                "\n    ;color: ${num_fg}"
                "\n    ;font-size: 8pt"
                );
    m_theme["tab_bar_sel_item_counter"] = Option(
                "\n    ;color: ${sel_bg}"
                );

    m_theme["tab_tree_css"] = Option(
                "\n    ;color: ${fg}"
                "\n    ;background-color: ${bg}"
                "\n    ;padding: .20em"
                );
    m_theme["tab_tree_sel_item_css"] = Option(
                "\n    ;color: ${fg}"
                "\n    ;background: ${midlight}"
                "\n    ;border-radius: 4px"
                );
    m_theme["tab_tree_item_counter"] = Option(
                "\n    ;padding: .33em"
                "\n    ;color: ${num_fg}"
                "\n    ;font-size: 8pt"
                );
    m_theme["tab_tree_sel_item_counter"] = Option(
                "\n    ;color: ${sel_bg}"
                );

    m_theme["tool_bar_css"] = Option(
                "\n    ;color: ${fg}"
                "\n    ;background-color: ${bg}"
                "\n    ;border: 0"
                "\n    ;border-left: 1px solid ${divider}"
                );
    m_theme["tool_button_css"] = Option(
                "\n    ;background: transparent"
                "\n    ;color: ${fg}"
                "\n    ;border: 0"
                "\n    ;border-radius: 4px"
                "\n    ;padding: 4px"
                );
    m_theme["tool_button_selected_css"] = Option(
                "\n    ;background: ${midlight}"
                "\n    ;color: ${fg}"
                "\n    ;border: 0"
                "\n    ;border-radius: 4px"
                );
    m_theme["tool_button_pressed_css"] = Option(
                "\n    ;background: ${mid}"
                "\n    ;border-radius: 4px"
                );

    m_theme["search_bar"] = Option(
                "\n    ;background: ${edit_bg}"
                "\n    ;color: ${edit_fg}"
                "\n    ;border: 1px solid ${mid}"
                "\n    ;border-radius: 4px"
                "\n    ;padding: 4px 8px"
                "\n    ;margin: 2px"
                );
    m_theme["search_bar_focused"] = Option(
                "\n    ;border: 1px solid ${sel_bg}"
                "\n    ;border-bottom: 2px solid ${sel_bg}"
                );

    m_theme["use_system_icons"] = Option(false, "checked", ui ? ui->checkBoxSystemIcons : nullptr);
    m_theme["font_antialiasing"] = Option(true, "checked", ui ? ui->checkBoxAntialias : nullptr);
    m_theme["style_main_window"] = Option(false, "checked", ui ? ui->checkBoxStyleMainWindow : nullptr);

    const int iconSize = iconFontSizePixels();
    m_theme["icon_size"] = Option( QString::number(iconSize) );

    m_theme["css_template_items"] = Option("items");
    m_theme["css_template_main_window"] = Option("main_window");
    m_theme["css_template_notification"] = Option("notification");
    m_theme["css_template_menu"] = Option("menu");

    m_theme["num_margin"] = Option(2);
    m_theme["num_sel_fg"] = Option(QString());
}

void Theme::updateTheme()
{
    m_margins = QSize(2, 2);

    // search style
    m_searchPalette.setColor(QPalette::Base, color("find_bg"));
    m_searchPalette.setColor(QPalette::Text, color("find_fg"));
    m_searchFont = font("find_font");

    // editor style
    m_editorPalette.setColor(QPalette::Base, color("edit_bg"));
    m_editorPalette.setColor(QPalette::Text, color("edit_fg"));
    m_editorFont = font("edit_font");

    // number style
    m_showRowNumber = value("show_number").toBool();
    m_rowNumberPalette.setColor(QPalette::Text, color("num_fg"));
    const QString numSelFgValue = value("num_sel_fg").toString();
    const QColor numSelFg =
        numSelFgValue.isEmpty() ? color("sel_fg") : evalColor(numSelFgValue, *this);
    m_rowNumberPalette.setColor(QPalette::HighlightedText, numSelFg);
    m_rowNumberFont = font("num_font");
    m_rowNumberFontMetrics = QFontMetrics(m_rowNumberFont);
    const QVariant rowNumberMargin = value("num_margin");
    m_rowNumberMargin = rowNumberMargin.canConvert<int>() ? rowNumberMargin.toInt() : 2;

    m_antialiasing = value("font_antialiasing").toBool();
}

QSize Theme::rowNumberSize(int n) const
{
    if (!m_showRowNumber)
        return QSize(0, 0);

    const auto number = QString::number(n + m_rowIndexFromOne);
    return m_rowNumberFontMetrics.boundingRect(number).size()
        + m_margins + QSize(m_rowNumberMargin, 0);
}

void Theme::decorateBrowser(QAbstractScrollArea *c) const
{
    decorateScrollArea(c);
    const QString cssTemplate = value("css_template_items").toString();
    c->setStyleSheet(getStyleSheet(cssTemplate));
}

bool Theme::isMainWindowThemeEnabled() const
{
    return m_theme.value("style_main_window").value().toBool();
}

QString Theme::themeColorString(const QString &name) const
{
    return serializeColor( color(name) );
}

QString Theme::getStyleSheet(const QString &name, Values values, int maxRecursion) const
{
    const QString fileName = findThemeFile(name + ".css");

    if ( fileName.isEmpty() )
        return QString();

    QFile file(fileName);
    if ( !file.open(QIODevice::ReadOnly) ) {
        log( QString("Failed to open stylesheet \"%1\": %2")
             .arg(fileName, file.errorString()), LogError );
        return QString();
    }

    const auto css = QString::fromUtf8( file.readAll() );
    return parseStyleSheet(css, values, maxRecursion - 1);
}

QString Theme::parseStyleSheet(const QString &css, Values values, int maxRecursion) const
{
    QString output;
    const QString variableBegin("${");
    const QString variableEnd("}");
    for ( int i = 0; i < css.size(); ++i ) {
        const int a = css.indexOf(variableBegin, i);
        if (a == -1) {
            output.append(css.mid(i));
            break;
        }

        const int b = css.indexOf(variableEnd, a + variableBegin.size());
        if (b == -1) {
            output.append(css.mid(i));
            break;
        }

        output.append(css.mid(i, a - i));
        i = b + variableEnd.size() - 1;

        const QString name = css
                .mid(a + variableBegin.size(), b - a - variableBegin.size())
                .trimmed();
        const QString result = parsePlaceholder(name, &values, maxRecursion);
        output.append(result);
    }

    return output;
}

QString Theme::parsePlaceholder(const QString &name, Values *values, int maxRecursion) const
{
    if (maxRecursion < 0) {
        log("Max recursion reached when parsing style sheet", LogError);
        return QString();
    }

    if ( name.contains("=") ) {
        const int i = name.indexOf('=');
        const QString k = name.mid(0, i).trimmed();
        const QString v = name.mid(i + 1).trimmed();
        if (k == "scale")
            (*values)[k] = 1.0;
        (*values)[k] = parsePlaceholder(v, values, maxRecursion - 1);
        return QString();
    }

    if ( name.startsWith("css:") )
        return getStyleSheet(name.mid(4), *values, maxRecursion - 1);

    const double scale = getFactor("scale", *values);

    const QString value = this->value(name).toString();
    if (name.contains("font"))
        return getFontStyleSheet(value, scale);

    if (name.contains("css") || value.contains(":"))
        return parseStyleSheet(value, *values, maxRecursion - 1);

    {
        bool ok;
        const double v = name.toDouble(&ok);
        if (ok)
            return QString::number(v * scale);
    }

    {
        bool ok;
        const double v = value.toDouble(&ok);
        if (ok)
            return QString::number(v * scale);
    }

    QColor color = evalColor(name, *this, *values);
    const double saturationF = getFactor("hsv_saturation_factor", *values);
    const double valueF = getFactor("hsv_value_factor", *values);
    if ( !qFuzzyCompare(saturationF, 1.0) || !qFuzzyCompare(valueF, 1.0) ) {
        color.setHsvF(
            color.hueF(),
            normalizeFactor(color.saturationF() * saturationF),
            normalizeFactor(color.valueF() * valueF),
            color.alphaF()
        );
    }
    return serializeColor(color);
}

QString serializeColor(const QColor &color)
{
    if (color.alpha() == 255)
        return color.name();

    return QString::fromLatin1("rgba(%1,%2,%3,%4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(color.alpha());
}

QColor deserializeColor(const QString &colorName)
{
    QColor color;

    if ( colorName.startsWith(QStringLiteral("rgba(")) ) {
        QStringList list = colorName.mid(5, colorName.indexOf(')') - 5).split(',');
        int r = list.value(0).toInt();
        int g = list.value(1).toInt();
        int b = list.value(2).toInt();
        int a = static_cast<int>( list.value(3).toDouble() * 255 );

        color = QColor(r, g, b, a > 255 ? a / 255 : a);
    } else if ( colorName.startsWith(QStringLiteral("default_")) ) {
        const QPalette::ColorRole role = defaultColorVarToRole(colorName);
        color = QPalette().color(role);
    } else {
        color = QColor(colorName);
    }

    if ( !color.isValid() )
        log( QString("Failed to deserialize color \"%1\"").arg(colorName) );

    return color;
}

QColor evalColor(const QString &expression, const Theme &theme, const Values &values, int maxRecursion)
{
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;

    QStringList addList = QString(expression).remove(' ').split('+');
    for (const auto &add : addList) {
        QStringList subList = add.split('-');
        float multiply = 1;
        for (const auto &sub : subList) {
            addColor(sub, multiply, &r, &g, &b, &a, theme, values, maxRecursion);
            multiply = -1;
        }
    }

    return QColor(r, g, b, a);
}

QString defaultUserThemePath()
{
    return QDir::cleanPath(settingsDirectoryPath() + "/themes");
}

QStringList themePaths()
{
    QStringList paths;

    const QString customThemsPath = qEnvironmentVariable("COPYQ_THEME_PREFIX");
    if ( !customThemsPath.isEmpty() )
        paths.append(customThemsPath);

    const QString userThemesPath = defaultUserThemePath();
    QDir themesDir(userThemesPath);
    if ( themesDir.mkpath(".") )
        paths.append(userThemesPath);

    const QString themesPath = themePrefix();
    if ( !themesPath.isEmpty() )
        paths.append(themesPath);

    return paths;
}

QString findThemeFile(const QString &fileName)
{
    for (const QString &path : themePaths()) {
        const QDir dir(path);
        if ( dir.exists(fileName) )
            return dir.absoluteFilePath(fileName);
    }
    return QString();
}
