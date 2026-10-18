#pragma once

// ---------------------------------------------------------------------------
// DeviceListPanel.h - the fleet list: search, filter, sort and a rich row.
//
// Why a custom delegate instead of QListWidgetItem text
// -----------------------------------------------------
// One row has to carry five things - type glyph, name, id, live temperature and
// a state lamp - and it has to do so in about 52 px so that twenty devices fit
// on a laptop screen without scrolling.  That is a layout, and a layout belongs
// in paint(), not in a string with tabs in it.  The delegate also means the row
// height and the colour of every element are decided in one place, which is what
// makes the list look like the rest of the application.
//
// The panel owns no domain state.  It reads through DeviceListModel and reports
// selection through one signal; every action the operator can take on a device
// is handled by the controller, so this file contains no business logic.
// ---------------------------------------------------------------------------

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QToolButton;

namespace saim {

class DeviceManager;
class DeviceListModel;
class DeviceFilterProxy;

class DeviceListPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit DeviceListPanel(DeviceManager* manager, QWidget* parent = nullptr);
    ~DeviceListPanel() override;

    /// The currently selected device id, or empty.  The controller uses this to
    /// decide what the dashboard and the AI panel are showing.
    [[nodiscard]] QString selectedDeviceId() const;

    /// Selects a device by id, scrolling it into view.  Used when the operator
    /// clicks an alarm in the log or a lamp on the overview.
    void selectDevice(const QString& deviceId);

    /// Called by the controller on the UI refresh tick.  Re-publishes live values
    /// without a model reset.
    void refreshValues();

    [[nodiscard]] DeviceListModel* model() const noexcept { return m_model; }

signals:
    /// Emitted on every selection change; empty id means "nothing selected".
    void deviceSelected(const QString& deviceId);

    /// Emitted on double-click - the controller interprets this as "open the
    /// device's detail view".
    void deviceActivated(const QString& deviceId);

    /// The filter or sort changed, so the header count may need re-reading.
    void filterChanged();

public slots:
    /// Applies a device-type filter from outside (the overview's type chips).
    void setTypeFilter(const QString& deviceType);

    /// Shows only devices in Warning or worse.
    void setOnlyProblems(bool onlyProblems);

private slots:
    void onSelectionChanged();
    void onSortIndexChanged(int index);
    void onSearchTextChanged(const QString& text);
    void updateHeaderCount();

private:
    void buildUi();
    void wireSignals();

    DeviceManager*      m_manager = nullptr;
    DeviceListModel*    m_model   = nullptr;
    DeviceFilterProxy*  m_proxy   = nullptr;

    QLineEdit*  m_search      = nullptr;
    QToolButton* m_problemToggle = nullptr;
    QComboBox*  m_sortBox     = nullptr;
    QListView*  m_view        = nullptr;
    QLabel*     m_countLabel  = nullptr;
    QLabel*     m_emptyLabel  = nullptr;
};

}   // namespace saim
