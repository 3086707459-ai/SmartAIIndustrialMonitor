#pragma once

// ---------------------------------------------------------------------------
// Theme.h - the single place that knows what the interface looks like.
//
// The rule this enforces
// ----------------------
// No widget builds a colour string, a font, or a stylesheet.  A widget asks
// Theme for a QColor and applies it; the palette itself is loaded once from a
// Qt resource and can be swapped at run time.  Three consequences:
//
//   * "Make the alarm colour less orange" is a one-line change in one file, not
//     a search across twenty widgets.
//   * The exported HTML report and the on-screen dashboard draw from the same
//     palette constants, so a printed report looks like the product it came from.
//   * A light theme is a second .qss file plus a second colour table - not a
//     refactor.  That matters because plant offices and plant floors want
//     different things, and the answer should not be "rewrite the UI".
//
// Why the stylesheet is loaded from a resource and not composed in C++
// -------------------------------------------------------------------
// A .qss file gets syntax highlighting, can be diffed, and can be edited by
// someone who does not read C++.  A stylesheet assembled from QStringLiterals
// gets none of that, and every change needs a rebuild.
// ---------------------------------------------------------------------------

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace saim {

/// Semantic colours.  These names describe *meaning*, so a widget asks for
/// `Theme::warning()` rather than for "amber" - and a re-theme changes what
/// warning looks like without touching a single call site.
namespace Theme {

// -- semantic status --------------------------------------------------------

[[nodiscard]] QColor normal();
[[nodiscard]] QColor warning();
[[nodiscard]] QColor fault();
[[nodiscard]] QColor offline();

/// Maps a device state onto its semantic colour.  The one function the UI
/// should call for a state colour - it exists so that the mapping cannot be
/// reimplemented, slightly differently, in five widgets.
[[nodiscard]] QColor stateColor(int deviceState);
[[nodiscard]] QColor levelColor(int anomalyLevel);

/// Colour for a 0..100 risk score, matching the report generator's thresholds.
[[nodiscard]] QColor riskColor(double riskScore);

// -- structural -------------------------------------------------------------

[[nodiscard]] QColor background();
[[nodiscard]] QColor surface();
[[nodiscard]] QColor surfaceRaised();
[[nodiscard]] QColor border();
[[nodiscard]] QColor textPrimary();
[[nodiscard]] QColor textSecondary();
[[nodiscard]] QColor textMuted();
[[nodiscard]] QColor accent();
[[nodiscard]] QColor gridLine();

// -- metrics ----------------------------------------------------------------
//
// Spacing and sizing constants, so that two panels built a week apart still line
// up.  A UI whose gaps are 6, 7 and 8 px because three people typed them by hand
// looks wrong in a way nobody can name.

namespace Metrics {
inline constexpr int kPanelSpacing   = 8;
inline constexpr int kPanelPadding   = 12;
inline constexpr int kRowHeight      = 26;
inline constexpr int kToolbarHeight  = 38;
inline constexpr int kStatusBarHeight = 24;
inline constexpr int kSidebarMinWidth = 240;
inline constexpr int kSidebarMaxWidth = 400;
inline constexpr int kRightPanelMinWidth = 300;
inline constexpr int kRightPanelMaxWidth = 460;
}   // namespace Metrics

/// Installs the application-wide palette and stylesheet.  Call once, after the
/// QApplication exists and before the first window is shown - applying a
/// stylesheet after widgets are visible causes a full re-polish and a visible
/// flicker.
void apply(QApplication& app);

/// Reloads the stylesheet from disk (a path override exists so a designer can
/// iterate on the .qss without a rebuild: SAIM_QSS=/path/to/file.qss).
/// Returns false if the file could not be read; the previous sheet stays.
[[nodiscard]] bool reload(QApplication& app);

/// Icon by resource name, e.g. icon("device-motor").  Returns a null icon for
/// an unknown name rather than a placeholder image, so a missing icon shows as
/// missing during development instead of as a mysterious grey square.
[[nodiscard]] QIcon icon(const QString& name);

/// The icon name for a device type, so the device list, the dashboard cards and
/// the report all pick the same glyph for a pump.
[[nodiscard]] QString iconForDeviceType(const QString& deviceType);

/// Font used for numeric readouts.  Monospaced digits matter on a value that
/// updates ten times a second: proportional digits make the number visibly
/// jitter as its width changes.
[[nodiscard]] QString numericFontFamily();

}   // namespace Theme
}   // namespace saim
