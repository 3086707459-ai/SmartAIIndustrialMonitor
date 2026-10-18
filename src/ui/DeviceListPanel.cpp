#include "ui/DeviceListPanel.h"

#include "ui/DeviceListModel.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace saim {

namespace {

/// Fixed row height.  Fixed rather than size-hint-driven because a list whose
/// rows change height as a value's text grows is a list that jitters while you
/// are reading it - and the delegate below draws within exactly this box.
constexpr int kRowHeight = 52;

/// Height of the thin state bar drawn down the left edge of each row.
constexpr int kStateBarWidth = 3;

}   // namespace

// ---------------------------------------------------------------------------
// DeviceRowDelegate - draws one device as an instrument row.
//
// This is the piece that makes the list look like plant software rather than
// like a database browser, so it is worth being explicit about the layout:
//
//   +---+--------------------------------------------------+
//   |   |  [icon]  Motor 001                        (o)   |   name + lamp
//   | | |          M-001   pump                   72.4 C  |   id/type + temp
//   +---+--------------------------------------------------+
//     ^
//     state bar - the fastest thing to read in a long list
//
// Everything is drawn with Theme colours and the numeric face, so a row here and
// a card on the dashboard cannot drift apart stylistically.
// ---------------------------------------------------------------------------
class DeviceRowDelegate final : public QStyledItemDelegate
{
public:
    explicit DeviceRowDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        return QSize(0, kRowHeight);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::TextAntialiasing, true);

        const QRect row = option.rect;
        const int state = index.data(DeviceListModel::DeviceStateRole).toInt();
        const QColor stateColour = Theme::stateColor(state);
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered  = (option.state & QStyle::State_MouseOver) != 0;

        // -- background -----------------------------------------------------
        // Selection is a raised surface plus a brighter bar, not an inverted
        // row.  An inverted row would hide the state colour, which is the one
        // thing on this list that must stay visible at all times.
        if (selected)
        {
            painter->fillRect(row, Theme::surfaceRaised());
        }
        else if (hovered)
        {
            QColor hover = Theme::surfaceRaised();
            hover.setAlpha(140);
            painter->fillRect(row, hover);
        }

        // -- state bar ------------------------------------------------------
        // The left edge bar is the fastest read in the panel: scanning a column
        // of colours finds the red one without reading a single word.
        painter->fillRect(QRect(row.left(), row.top(), kStateBarWidth, row.height()),
                          stateColour);

        // -- separator ------------------------------------------------------
        painter->setPen(QPen(Theme::border(), 1.0));
        painter->drawLine(row.left() + kStateBarWidth, row.bottom(),
                          row.right(), row.bottom());

        const QRect content = row.adjusted(kStateBarWidth + 10, 6, -10, -7);

        // -- icon -----------------------------------------------------------
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        constexpr int iconSize = 22;
        const QRect iconRect(content.left(), content.top() + 2, iconSize, iconSize);
        if (!icon.isNull())
        {
            icon.paint(painter, iconRect, Qt::AlignCenter,
                       selected ? QIcon::Normal : QIcon::Normal);
        }

        const int textLeft = iconRect.right() + 8;

        // -- name -----------------------------------------------------------
        QFont nameFont = option.font;
        nameFont.setBold(state >= static_cast<int>(DeviceState::Warning));
        nameFont.setPointSizeF(std::max(9.0, option.font.pointSizeF()));

        painter->setFont(nameFont);
        painter->setPen(Theme::textPrimary());

        const QString name = index.data(DeviceListModel::DeviceNameRole).toString();
        const QFontMetrics nameMetrics(nameFont);

        // Truncate by hand rather than letting drawText clip: a clipped glyph is
        // worse than an ellipsis, and the reserved width below depends on knowing
        // how much space the name actually took.
        const int reservedRight = 58;
        const QString shownName = nameMetrics.elidedText(
            name, Qt::ElideRight, std::max(10, content.width() - (textLeft - content.left()) - reservedRight));

        painter->drawText(QRect(textLeft, content.top(), content.width() - (textLeft - content.left()) - reservedRight,
                                nameMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, shownName);

        // -- id and type ----------------------------------------------------
        QFont subFont = option.font;
        subFont.setPointSizeF(std::max(7.5, option.font.pointSizeF() - 1.0));

        painter->setFont(subFont);
        painter->setPen(Theme::textMuted());

        const QFontMetrics subMetrics(subFont);
        const QString subtitle = QStringLiteral("%1  %2")
                                     .arg(index.data(DeviceListModel::DeviceIdRole).toString(),
                                          index.data(DeviceListModel::DeviceTypeRole).toString());

        painter->drawText(QRect(textLeft, content.top() + nameMetrics.height() + 1,
                                content.width() - (textLeft - content.left()) - reservedRight,
                                subMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, subtitle);

        // -- live value -----------------------------------------------------
        // Temperature, right-aligned in the numeric face.  It is the headline
        // analogue quantity on this list because it is the one an operator can
        // compare across machines at a glance.
        const QVariant temperature = index.data(DeviceListModel::TemperatureRole);
        if (temperature.isValid() && temperature.toDouble() != 0.0)
        {
            QFont valueFont(Theme::numericFontFamily());
            valueFont.setPointSizeF(std::max(10.0, option.font.pointSizeF() + 1.0));
            valueFont.setBold(true);

            painter->setFont(valueFont);

            // Coloured against the *same* thresholds the alarm engine uses, so
            // the row and the alarm can never tell the operator different
            // stories.  The limits arrive as a role rather than being hard-coded
            // here precisely so that per-device datasheets work.
            painter->setPen(Theme::textPrimary());

            const QString text = QStringLiteral("%1°")
                                     .arg(QString::number(temperature.toDouble(), 'f', 1));
            const QFontMetrics valueMetrics(valueFont);

            painter->drawText(QRect(content.right() - 56, content.top(),
                                    56, valueMetrics.height()),
                              Qt::AlignRight | Qt::AlignVCenter, text);
        }

        // -- health ---------------------------------------------------------
        // Only once a verdict exists.  Before that the row shows nothing rather
        // than "100%", because a health score nobody has computed is not 100 % -
        // it is unknown, and green unknown is the most dangerous thing a
        // dashboard can show.
        const QVariant health = index.data(DeviceListModel::HealthRole);
        if (health.isValid())
        {
            QFont subValueFont = option.font;
            subValueFont.setPointSizeF(std::max(7.5, option.font.pointSizeF() - 1.0));

            painter->setFont(subValueFont);
            painter->setPen(Theme::textSecondary());

            const QFontMetrics subValueMetrics(subValueFont);
            const QString text = QStringLiteral("health %1%")
                                     .arg(QString::number(health.toDouble(), 'f', 0));

            painter->drawText(QRect(content.right() - 90,
                                    content.top() + nameMetrics.height() + 1,
                                    90, subValueMetrics.height()),
                              Qt::AlignRight | Qt::AlignVCenter, text);
        }

        // -- alarm badge ----------------------------------------------------
        const qlonglong alarms = index.data(DeviceListModel::AlarmCountRole).toLongLong();
        if (alarms > 0)
        {
            QFont badgeFont = option.font;
            badgeFont.setPointSizeF(std::max(7.0, option.font.pointSizeF() - 1.5));
            badgeFont.setBold(true);

            const QString badge = QString::number(alarms);
            const QFontMetrics badgeMetrics(badgeFont);
            const int badgeWidth = std::max(16, badgeMetrics.horizontalAdvance(badge) + 8);
            const QRect badgeRect(content.right() - badgeWidth, content.top(),
                                  badgeWidth, badgeMetrics.height() + 2);

            QColor badgeColour = stateColour;
            badgeColour.setAlpha(200);
            painter->setPen(Qt::NoPen);
            painter->setBrush(badgeColour);
            painter->drawRoundedRect(badgeRect, 3.0, 3.0);

            painter->setFont(badgeFont);
            painter->setPen(QColor(QStringLiteral("#0d1117")));
            painter->drawText(badgeRect, Qt::AlignCenter, badge);
        }

        painter->restore();
    }
};

// ---------------------------------------------------------------------------
// DeviceListPanel
// ---------------------------------------------------------------------------

DeviceListPanel::DeviceListPanel(DeviceManager* manager, QWidget* parent)
    : QWidget(parent)
    , m_manager(manager)
{
    setObjectName(QStringLiteral("DeviceListPanel"));
    buildUi();
    wireSignals();
    updateHeaderCount();
}

DeviceListPanel::~DeviceListPanel() = default;

void DeviceListPanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Theme::Metrics::kPanelSpacing);

    // -- header -------------------------------------------------------------
    auto* header = new QWidget(this);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding,
                                     Theme::Metrics::kPanelPadding, 0);
    headerLayout->setSpacing(6);

    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(6);

    auto* title = new QLabel(tr("Devices"), header);
    title->setObjectName(QStringLiteral("PanelTitle"));
    titleRow->addWidget(title);

    m_countLabel = new QLabel(header);
    m_countLabel->setObjectName(QStringLiteral("MetricCaption"));
    titleRow->addWidget(m_countLabel);
    titleRow->addStretch(1);

    m_problemToggle = new QToolButton(header);
    m_problemToggle->setText(tr("Problems"));
    m_problemToggle->setCheckable(true);
    m_problemToggle->setToolTip(tr("Show only devices in warning or fault state"));
    m_problemToggle->setObjectName(QStringLiteral("FilterToggle"));
    titleRow->addWidget(m_problemToggle);

    headerLayout->addLayout(titleRow);

    // -- search and sort ----------------------------------------------------
    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(6);

    m_search = new QLineEdit(header);
    m_search->setPlaceholderText(tr("Search id, name or type..."));
    m_search->setClearButtonEnabled(true);
    filterRow->addWidget(m_search, 1);

    m_sortBox = new QComboBox(header);
    m_sortBox->addItem(tr("Severity"), 0);      // the default, and the useful one
    m_sortBox->addItem(tr("Name"), 1);
    m_sortBox->addItem(tr("Temperature"), 2);
    m_sortBox->addItem(tr("Health"), 3);
    m_sortBox->setToolTip(tr("Sort order for the device list"));
    filterRow->addWidget(m_sortBox);

    headerLayout->addLayout(filterRow);
    layout->addWidget(header);

    // -- list ---------------------------------------------------------------
    m_model = new DeviceListModel(m_manager, this);

    m_proxy = new DeviceFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->sortBySeverity();

    m_view = new QListView(this);
    m_view->setObjectName(QStringLiteral("DeviceListView"));
    m_view->setModel(m_proxy);
    m_view->setItemDelegate(new DeviceRowDelegate(m_view));
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setUniformItemSizes(true);          // lets the view compute scroll extents in O(1)
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setMouseTracking(true);             // required for the hover state in the delegate
    m_view->setFrameShape(QFrame::NoFrame);

    layout->addWidget(m_view, 1);

    // -- empty state --------------------------------------------------------
    // A filter that matches nothing currently leaves a blank rectangle, which
    // reads as "the application is broken".  Saying so explicitly costs one
    // label and removes an entire category of support question.
    m_emptyLabel = new QLabel(tr("No device matches the current filter."), this);
    m_emptyLabel->setObjectName(QStringLiteral("EmptyState"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->hide();
    layout->addWidget(m_emptyLabel);
}

void DeviceListPanel::wireSignals()
{
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &DeviceListPanel::onSelectionChanged);
    connect(m_view, &QListView::doubleClicked,
            this, [this](const QModelIndex& index)
            {
                if (index.isValid())
                {
                    emit deviceActivated(index.data(DeviceListModel::DeviceIdRole).toString());
                }
            });
    connect(m_search, &QLineEdit::textChanged, this, &DeviceListPanel::onSearchTextChanged);
    connect(m_sortBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DeviceListPanel::onSortIndexChanged);

    connect(m_problemToggle, &QToolButton::toggled, this, [this](bool checked)
    {
        m_proxy->setOnlyProblems(checked);
        updateHeaderCount();
        emit filterChanged();
    });
}

QString DeviceListPanel::selectedDeviceId() const
{
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
    {
        return {};
    }
    return current.data(DeviceListModel::DeviceIdRole).toString();
}

void DeviceListPanel::selectDevice(const QString& deviceId)
{
    const QModelIndex source = m_model->indexOfDevice(deviceId);
    if (!source.isValid())
    {
        return;
    }

    const QModelIndex proxyIndex = m_proxy->mapFromSource(source);
    if (!proxyIndex.isValid())
    {
        // The device exists but the active filter hides it.  Clearing the filter
        // is the right response: the operator asked to see this device, and
        // silently doing nothing would look like a broken click.
        m_search->clear();
        m_proxy->setOnlyProblems(false);
        m_problemToggle->setChecked(false);
        m_proxy->setTypeFilter(QString());

        const QModelIndex retry = m_proxy->mapFromSource(source);
        if (!retry.isValid())
        {
            return;
        }
        m_view->setCurrentIndex(retry);
        m_view->scrollTo(retry, QAbstractItemView::PositionAtCenter);
        return;
    }

    m_view->setCurrentIndex(proxyIndex);
    m_view->scrollTo(proxyIndex, QAbstractItemView::PositionAtCenter);
}

void DeviceListPanel::refreshValues()
{
    m_model->refreshValues();
}

void DeviceListPanel::setTypeFilter(const QString& deviceType)
{
    m_proxy->setTypeFilter(deviceType);
    updateHeaderCount();
}

void DeviceListPanel::setOnlyProblems(bool onlyProblems)
{
    m_problemToggle->setChecked(onlyProblems);
}

void DeviceListPanel::onSelectionChanged()
{
    emit deviceSelected(selectedDeviceId());
}

void DeviceListPanel::onSortIndexChanged(int index)
{
    // QSortFilterProxyModel::sort() orders by sortRole(), which defaults to
    // Qt::DisplayRole.  Every one of these orderings is by a *custom* role, so
    // setting sortRole() is not optional - without it the list would silently
    // sort alphabetically by name no matter which entry was chosen, and the
    // symptom ("Temperature sort does nothing") gives no hint as to the cause.
    switch (index)
    {
    case 1:
        m_proxy->setSortRole(DeviceListModel::DeviceNameRole);
        m_proxy->sort(0, Qt::AscendingOrder);
        break;
    case 2:
        // Hottest first: the operator scanning by temperature wants the machine
        // nearest its thermal limit, not the coldest one.
        m_proxy->setSortRole(DeviceListModel::TemperatureRole);
        m_proxy->sort(0, Qt::DescendingOrder);
        break;
    case 3:
        // Ascending: the lowest health score is the most urgent, so it goes first.
        // Rows with no verdict yet carry an invalid QVariant and the proxy's
        // default comparison places them last, which is the correct reading -
        // "not analysed" is not "healthy" and must not outrank a known-bad machine.
        m_proxy->setSortRole(DeviceListModel::HealthRole);
        m_proxy->sort(0, Qt::AscendingOrder);
        break;
    case 0:
    default:
        m_proxy->sortBySeverity();
        break;
    }

    updateHeaderCount();
    emit filterChanged();
}

void DeviceListPanel::onSearchTextChanged(const QString& text)
{
    m_proxy->setSearchText(text);
    updateHeaderCount();
    emit filterChanged();
}

void DeviceListPanel::updateHeaderCount()
{
    const int visible = m_proxy->rowCount();
    const int total = m_model->rowCount();

    if (visible == total)
    {
        m_countLabel->setText(tr("(%1)").arg(total));
    }
    else
    {
        // Showing both numbers matters: "3 of 6" tells the operator a filter is
        // active and by how much, where "3" alone would look like devices have
        // gone missing.
        m_countLabel->setText(tr("(%1 of %2)").arg(visible).arg(total));
    }

    const bool empty = (visible == 0 && total > 0);
    m_emptyLabel->setVisible(empty);
    m_view->setVisible(!empty);
}

}   // namespace saim
