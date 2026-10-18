#include "ui/Theme.h"

#include "common/Types.h"
#include "core/Logger.h"

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

namespace saim {
namespace Theme {

namespace {

/// The stylesheet actually applied, kept so reload() can fall back to it when a
/// replacement fails to load.  A broken .qss must never leave the application
/// unstyled - an unstyled Qt application is close to unusable, and the person who
/// introduced the syntax error would have no way to fix it from the UI.
QString g_activeStyleSheet;

/// Reads the built-in stylesheet, or an override from SAIM_QSS.
QString loadStyleSheet()
{
    const QString override = qEnvironmentVariable("SAIM_QSS");
    if (!override.isEmpty())
    {
        QFile file(override);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            SAIM_INFO(logcat::kUi,
                      QStringLiteral("Theme: stylesheet override loaded from %1").arg(override));
            return QString::fromUtf8(file.readAll());
        }
        SAIM_WARN(logcat::kUi,
                  QStringLiteral("Theme: SAIM_QSS=%1 could not be read; using the built-in "
                                 "stylesheet")
                      .arg(override));
    }

    // The resource path is stable: it is the alias declared in resources.qrc, so
    // renaming the file on disk does not break this line.
    QFile builtIn(QStringLiteral(":/theme/industrial-dark.qss"));
    if (builtIn.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return QString::fromUtf8(builtIn.readAll());
    }

    SAIM_ERROR(logcat::kUi, QStringLiteral("Theme: built-in stylesheet missing from resources"));
    return {};
}

}   // namespace

// ---------------------------------------------------------------------------
// Semantic status colours
//
// These are duplicated from the .qss on purpose, and the duplication is the one
// place in the project where it is justified: the stylesheet styles widgets, and
// these values are also needed by code that paints (chart series, the camera
// overlay, the HTML report) - which a stylesheet cannot reach.  Keeping them in
// one header means the two definitions sit four lines apart in review.
//
// The values match stateColor() in common/Types.cpp.
// ---------------------------------------------------------------------------

QColor normal()  { return QColor(QStringLiteral("#3fa46a")); }
QColor warning() { return QColor(QStringLiteral("#d9a13c")); }
QColor fault()   { return QColor(QStringLiteral("#d64545")); }
QColor offline() { return QColor(QStringLiteral("#6b7684")); }

QColor stateColor(int deviceState)
{
    // Intentional: routed through the domain enum rather than duplicating the
    // switch, so adding a state cannot leave the UI behind.
    return QColor(stateColor(static_cast<DeviceState>(deviceState)));
}

QColor levelColor(int anomalyLevel)
{
    return QColor(saim::levelColor(static_cast<AnomalyLevel>(anomalyLevel)));
}

QColor riskColor(double riskScore)
{
    // Thresholds mirror AIReportGenerator::riskLevelFor().  If those move, these
    // must move with them - which is why both carry the same comment.
    if (riskScore >= 70.0)
    {
        return fault();
    }
    if (riskScore >= 45.0)
    {
        return QColor(QStringLiteral("#e07b39"));
    }
    if (riskScore >= 20.0)
    {
        return warning();
    }
    return normal();
}

// ---------------------------------------------------------------------------
// Structural colours
// ---------------------------------------------------------------------------

QColor background()     { return QColor(QStringLiteral("#10141a")); }
QColor surface()        { return QColor(QStringLiteral("#161c24")); }
QColor surfaceRaised()  { return QColor(QStringLiteral("#1c2530")); }
QColor border()         { return QColor(QStringLiteral("#232c38")); }
QColor textPrimary()    { return QColor(QStringLiteral("#d6dde6")); }
QColor textSecondary()  { return QColor(QStringLiteral("#8b97a6")); }
QColor textMuted()      { return QColor(QStringLiteral("#63707f")); }
QColor accent()         { return QColor(QStringLiteral("#3d7fd6")); }
QColor gridLine()       { return QColor(QStringLiteral("#1e2732")); }

// ---------------------------------------------------------------------------
// Application-wide installation
// ---------------------------------------------------------------------------

void apply(QApplication& app)
{
    // Fusion first, and not optionally.  Qt's Windows style delegates parts of
    // its rendering - combo popups, scrollbar metrics, some focus rectangles - to
    // the native theme engine, and those parts ignore a stylesheet.  The result
    // is a mostly-dark application with a few light-grey artefacts that look like
    // bugs.  Fusion is fully stylesheet-driven, so what the .qss says is what is
    // drawn.
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // A dark QPalette as well as a stylesheet: the stylesheet covers the widgets
    // it names, the palette covers everything it does not - native dialogs, the
    // colour picker, tooltips from Qt itself, and the brief frame between widget
    // construction and first polish.
    QPalette palette;
    palette.setColor(QPalette::Window,          background());
    palette.setColor(QPalette::WindowText,      textPrimary());
    palette.setColor(QPalette::Base,            QColor(QStringLiteral("#0d1117")));
    palette.setColor(QPalette::AlternateBase,   surface());
    palette.setColor(QPalette::Text,            textPrimary());
    palette.setColor(QPalette::Button,          surfaceRaised());
    palette.setColor(QPalette::ButtonText,      textPrimary());
    palette.setColor(QPalette::BrightText,      fault());
    palette.setColor(QPalette::Highlight,       QColor(QStringLiteral("#2c5f9e")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::ToolTipBase,     surfaceRaised());
    palette.setColor(QPalette::ToolTipText,     textPrimary());
    palette.setColor(QPalette::Link,            accent());
    palette.setColor(QPalette::PlaceholderText, textMuted());

    // Disabled group, set explicitly.  Fusion's default disabled colours are
    // derived from the light palette and come out as unreadable dark grey on a
    // dark background.
    palette.setColor(QPalette::Disabled, QPalette::WindowText, textMuted());
    palette.setColor(QPalette::Disabled, QPalette::Text,       QColor(QStringLiteral("#4c5764")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#4c5764")));

    app.setPalette(palette);

    g_activeStyleSheet = loadStyleSheet();
    app.setStyleSheet(g_activeStyleSheet);

    SAIM_INFO(logcat::kUi,
              QStringLiteral("Theme applied: %1 (%2 characters of stylesheet)")
                  .arg(QStringLiteral("industrial-dark"))
                  .arg(g_activeStyleSheet.size()));
}

bool reload(QApplication& app)
{
    const QString candidate = loadStyleSheet();
    if (candidate.isEmpty())
    {
        SAIM_WARN(logcat::kUi,
                  QStringLiteral("Theme reload failed; keeping the previous stylesheet"));
        return false;
    }

    g_activeStyleSheet = candidate;
    app.setStyleSheet(g_activeStyleSheet);
    SAIM_INFO(logcat::kUi, QStringLiteral("Theme reloaded"));
    return true;
}

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------

QIcon icon(const QString& name)
{
    const QString path = QStringLiteral(":/theme/%1.svg").arg(name);

    // A null QIcon for a missing resource.  Qt's resource system returns an empty
    // image rather than failing, so without this check a typo'd icon name would
    // silently render as blank space.  Logging once per distinct name would be
    // better; the cost of the lookup is a hash probe, so it is done every time
    // and the log line is left at debug.
    const QIcon result(path);
    if (result.isNull())
    {
        SAIM_DEBUG(logcat::kUi, QStringLiteral("Theme: no icon resource at %1").arg(path));
    }
    return result;
}

QString iconForDeviceType(const QString& deviceType)
{
    const QString type = deviceType.toLower();

    // Substring matching rather than equality: a customer's device type is
    // whatever their historian calls it ("AC_MOTOR_3PH", "motor-75kW"), and an
    // exact-match table would fall back to a generic glyph for all of them.
    if (type.contains(QStringLiteral("motor")))
    {
        return QStringLiteral("device-motor");
    }
    if (type.contains(QStringLiteral("pump")))
    {
        return QStringLiteral("device-pump");
    }
    if (type.contains(QStringLiteral("spindle")) || type.contains(QStringLiteral("cnc")))
    {
        return QStringLiteral("device-spindle");
    }
    if (type.contains(QStringLiteral("fan")) || type.contains(QStringLiteral("blower")))
    {
        return QStringLiteral("device-fan");
    }
    return QStringLiteral("app");
}

QString numericFontFamily()
{
    // Consolas ships with Windows and Cascadia Mono with the Windows Terminal;
    // the monospace generic is the fallback for a machine with neither.  The
    // point is tabular figures, not the specific face.
    const QStringList candidates{
        QStringLiteral("Cascadia Mono"),
        QStringLiteral("Consolas"),
        QStringLiteral("DejaVu Sans Mono"),
        QStringLiteral("Menlo"),
        QStringLiteral("monospace"),
    };

    const QStringList families = QFontDatabase::families();
    for (const QString& candidate : candidates)
    {
        if (families.contains(candidate))
        {
            return candidate;
        }
    }
    return QStringLiteral("monospace");
}

}   // namespace Theme
}   // namespace saim
