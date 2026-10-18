# ---------------------------------------------------------------------------
# saim_ui - the presentation layer.
#
# Note what is *not* here: no SQL, no sockets, no inference.  The UI links
# saim::thread (which owns the workers) and saim::device (for the domain types),
# and talks to everything else through signals.  If a widget ever needs to reach
# a database or a model directly, that is the layering telling you the operation
# belongs in a worker.
#
# Note also what is deliberately *not* used: there is no .ui form file.  Qt
# Designer is the right tool for a form whose layout is the bulk of its value -
# a settings dialog, an about box.  Every panel here is either a custom-painted
# widget or a layout whose composition is the interesting part, and expressing
# that in XML would push the structure out of sight of code review while adding
# a build step.  QFormLayout is used for the parts that genuinely are forms.
#
# AUTOMOC/AUTOUIC/AUTORCC come from qt_standard_project_setup() at the top level.
# ---------------------------------------------------------------------------

set(SAIM_UI_SOURCES
    Theme.h
    Theme.cpp

    StateIndicator.h
    StateIndicator.cpp

    MetricCard.h
    MetricCard.cpp

    GaugeWidget.h
    GaugeWidget.cpp

    DeviceListModel.h
    DeviceListModel.cpp

    DeviceListPanel.h
    DeviceListPanel.cpp

    DeviceDetailPanel.h
    DeviceDetailPanel.cpp

    DashboardWidget.h
    DashboardWidget.cpp

    ChartWidget.h
    ChartWidget.cpp

    LogPanel.h
    LogPanel.cpp

    AIChatWidget.h
    AIChatWidget.cpp

    CameraWidget.h
    CameraWidget.cpp

    ReportDialog.h
    ReportDialog.cpp

    MainWindow.h
    MainWindow.cpp
)

# The theme lives in the UI layer because nothing below it draws anything.  All
# of it is one Qt resource target so the palette, the stylesheet and the icons
# are compiled into the executable - a deployed plant PC must not depend on a
# resources directory surviving next to the .exe.
#
# Note there is no hand-written .qrc.  qt_add_resources() generates the resource
# script from this list, so the file list lives in exactly one place; a checked-in
# .qrc alongside it would be a second source of truth that silently drifts.
#
# The call must come *after* the target exists.  Called before, Qt does not
# recognise `saim_ui` as a target and falls back to the legacy
# "qt_add_resources(<var> <file.qrc>...)" form - which then tries to parse the
# PREFIX value as a resource script, hence the odd errors at configure time.
saim_add_library(saim_ui
    SOURCES
        ${SAIM_UI_SOURCES}
    LIBS
        saim::common
        saim::core
        saim::device
        saim::thread
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Charts
        Qt6::Svg
)

qt_add_resources(saim_ui "saim_theme"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/theme/industrial-dark.qss"
        "${CMAKE_SOURCE_DIR}/resources/icons/app.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/device-motor.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/device-pump.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/device-spindle.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/device-fan.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/alarm.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/report.svg"
)
