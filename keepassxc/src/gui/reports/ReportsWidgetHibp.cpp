/*
 *  Copyright (C) 2020 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ReportsWidgetHibp.h"
#include "ui_ReportsWidgetHibp.h"

#include "config-keepassx.h"
#include "core/Database.h"
#include "core/Global.h"
#include "core/Group.h"
#include "core/PasswordHealth.h"
#include "gui/Icons.h"
#include "gui/MessageBox.h"

#include <QMenu>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>

namespace
{
    /*
     * Check if an entry has been marked as "known bad password".
     * These entries are to be excluded from the HIBP report.
     *
     * Question to reviewer: Should this be a member function of Entry?
     * It's duplicated in EditEntryWidget::setForms, EditEntryWidget::updateEntryData,
     * ReportsWidgetHealthcheck::customMenuRequested, and Health::Item::Item.
     */
    bool isKnownBad(const Entry* entry)
    {
        return entry->customData()->contains(PasswordHealth::OPTION_KNOWN_BAD)
               && entry->customData()->value(PasswordHealth::OPTION_KNOWN_BAD) == TRUE_STR;
    }

    class ReportSortProxyModel : public QSortFilterProxyModel
    {
    public:
        ReportSortProxyModel(QObject* parent)
            : QSortFilterProxyModel(parent){};
        ~ReportSortProxyModel() override = default;

    protected:
        bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
        {
            // Sort count column by user data
            if (left.column() == 2) {
                return sourceModel()->data(left, Qt::UserRole).toInt()
                       < sourceModel()->data(right, Qt::UserRole).toInt();
            }
            // Otherwise use default sorting
            return QSortFilterProxyModel::lessThan(left, right);
        }
    };
} // namespace

ReportsWidgetHibp::ReportsWidgetHibp(QWidget* parent)
    : QWidget(parent)
    , m_ui(new Ui::ReportsWidgetHibp())
    , m_referencesModel(new QStandardItemModel(this))
    , m_modelProxy(new ReportSortProxyModel(this))
{
    m_ui->setupUi(this);

    m_modelProxy->setSourceModel(m_referencesModel.data());
    m_modelProxy->setSortLocaleAware(true);
    m_ui->hibpTableView->setModel(m_modelProxy.data());
    m_ui->hibpTableView->setSelectionMode(QAbstractItemView::NoSelection);
    m_ui->hibpTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_ui->hibpTableView->setSortingEnabled(true);

    connect(m_ui->hibpTableView, SIGNAL(doubleClicked(QModelIndex)), SLOT(emitEntryActivated(QModelIndex)));
    connect(m_ui->hibpTableView, SIGNAL(customContextMenuRequested(QPoint)), SLOT(customMenuRequested(QPoint)));
    connect(m_ui->showKnownBadCheckBox, SIGNAL(stateChanged(int)), this, SLOT(makeHibpTable()));
#ifdef WITH_XC_NETWORKING
    connect(&m_downloader, SIGNAL(hibpResult(QString, int)), SLOT(addHibpResult(QString, int)));
    connect(&m_downloader, SIGNAL(fetchFailed(QString)), SLOT(fetchFailed(QString)));

    connect(m_ui->validationButton, &QPushButton::pressed, [this] { startValidation(); });
#endif
}

ReportsWidgetHibp::~ReportsWidgetHibp()
{
}

void ReportsWidgetHibp::loadSettings(QSharedPointer<Database> db)
{
    // Re-initialize
    m_db = std::move(db);
    m_referencesModel->clear();
    m_pwndPasswords.clear();
    m_error.clear();
    m_rowToEntry.clear();
    m_editedEntry = nullptr;
#ifdef WITH_XC_NETWORKING
    m_ui->stackedWidget->setCurrentIndex(0);
    m_ui->validationButton->setEnabled(true);
    m_ui->progressBar->hide();
#else
    // Compiled without networking, can't do anything
    m_ui->stackedWidget->setCurrentIndex(2);
#endif
}

/*
 * Fill the table will all entries that have passwords that we've
 * found to have been pwned.
 */
void ReportsWidgetHibp::makeHibpTable()
{
    // Reset the table
    m_referencesModel->clear();
    m_rowToEntry.clear();

    // If there were no findings, display a motivational message
    if (m_pwndPasswords.isEmpty() && m_error.isEmpty()) {
        m_referencesModel->setHorizontalHeaderLabels(QStringList() << tr("Congratulations, no exposed passwords!"));
        m_ui->stackedWidget->setCurrentIndex(1);
        return;
    }

    // Standard header labels for found issues
    m_referencesModel->setHorizontalHeaderLabels(QStringList() << tr("Title") << tr("Path") << tr("Password exposed???"));

    // Search database for passwords that we've found so far
    QList<QPair<const Entry*, int>> items;
    for (const auto* entry : m_db->rootGroup()->entriesRecursive()) {
        if (!entry->isRecycled()) {
            const auto found = m_pwndPasswords.find(entry->password());
            if (found != m_pwndPasswords.end()) {
                items.append({entry, found.value()});
            }
        }
    }

    // Sort decending by the number the password has been exposed
    qSort(items.begin(), items.end(), [](QPair<const Entry*, int>& lhs, QPair<const Entry*, int>& rhs) {
        return lhs.second > rhs.second;
    });

    // Display entries that are marked as "known bad"?
    const auto showKnownBad = m_ui->showKnownBadCheckBox->isChecked();

    // The colors for table cells
    const auto red = QBrush("red");

    // Build the table
    bool anyKnownBad = false;
    for (const auto& item : items) {
        const auto entry = item.first;
        const auto group = entry->group();
        const auto count = item.second;
        auto title = entry->title();

        // If the entry is marked as known bad, hide it unless the
        // checkbox is set.
        bool knownBad = isKnownBad(entry);
        if (knownBad) {
            anyKnownBad = true;
            if (!showKnownBad) {
                continue;
            }

            title.append(tr(" (Excluded)"));
        }

        auto row = QList<QStandardItem*>();
        row << new QStandardItem(entry->iconPixmap(), title)
            << new QStandardItem(group->iconPixmap(), group->hierarchy().join("/"))
            << new QStandardItem(countToText(count));

        if (knownBad) {
            row[1]->setToolTip(tr("This entry is being excluded from reports"));
        }

        row[2]->setForeground(red);
        row[2]->setData(count, Qt::UserRole);
        m_referencesModel->appendRow(row);

        // Store entry pointer per table row (used in double click handler)
        m_rowToEntry.append(entry);
    }

    // If there was an error, append the error message to the table
    if (!m_error.isEmpty()) {
        auto row = QList<QStandardItem*>();
        row << new QStandardItem(m_error);
        m_referencesModel->appendRow(row);
        row[0]->setForeground(QBrush(QColor("red")));
    }

    // If we're done and everything is good, display a motivational message
#ifdef WITH_XC_NETWORKING
    if (m_downloader.passwordsRemaining() == 0 && m_pwndPasswords.isEmpty() && m_error.isEmpty()) {
        m_referencesModel->clear();
        m_referencesModel->setHorizontalHeaderLabels(QStringList() << tr("Congratulations, no exposed passwords!"));
    }
#endif

    // Show the "show known bad entries" checkbox if there's any known
    // bad entry in the database.
    if (anyKnownBad) {
        m_ui->showKnownBadCheckBox->show();
    } else {
        m_ui->showKnownBadCheckBox->hide();
    }

    m_ui->hibpTableView->resizeRowsToContents();
    m_ui->hibpTableView->sortByColumn(2, Qt::DescendingOrder);

    m_ui->stackedWidget->setCurrentIndex(1);
}

/*
 * Invoked when the downloader has finished checking one password.
 */
void ReportsWidgetHibp::addHibpResult(const QString& password, int count)
{
    // Add the password to the list of our findings if it has been pwned
    if (count > 0) {
        m_pwndPasswords[password] = count;
    }

#ifdef WITH_XC_NETWORKING
    // Update the progress bar
    int remaining = m_downloader.passwordsRemaining();
    if (remaining > 0) {
        m_ui->progressBar->setValue(m_ui->progressBar->maximum() - remaining);
    } else {
        // Finished, remove the progress bar and build the table
        m_ui->progressBar->hide();
        makeHibpTable();
    }
#endif
}

/*
 * Invoked when a query to the HIBP server fails.
 *
 * Displays the table with the current findings.
 */
void ReportsWidgetHibp::fetchFailed(const QString& error)
{
    m_error = error;
    m_ui->progressBar->hide();
    makeHibpTable();
}

/*
 * Add passwords to the downloader and start the actual online validation.
 */
void ReportsWidgetHibp::startValidation()
{
#ifdef WITH_XC_NETWORKING
    // Collect all passwords in the database (unless recycled, and
    // unless empty, and unless marked as "known bad") and submit them
    // to the downloader.
    for (const auto* entry : m_db->rootGroup()->entriesRecursive()) {
        if (!entry->isRecycled() && !entry->password().isEmpty()) {
            m_downloader.add(entry->password());
        }
    }

    // Short circuit if we didn't actually add any passwords
    if (m_downloader.passwordsToValidate() == 0) {
        makeHibpTable();
        return;
    }

    // Store the number of passwords we need to check for the progress bar
    m_ui->progressBar->show();
    m_ui->progressBar->setMaximum(m_downloader.passwordsToValidate());
    m_ui->validationButton->setEnabled(false);

    m_downloader.validate();
#endif
}

/*
 * Convert the number of times a password has been pwned into
 * a display text for the third table column.
 */
QString ReportsWidgetHibp::countToText(int count)
{
    if (count == 1) {
        return tr("once");
    } else if (count <= 10) {
        return tr("up to 10 times");
    } else if (count <= 100) {
        return tr("up to 100 times");
    } else if (count <= 1000) {
        return tr("up to 1000 times");
    } else if (count <= 10000) {
        return tr("up to 10,000 times");
    } else if (count <= 100000) {
        return tr("up to 100,000 times");
    } else if (count <= 1000000) {
        return tr("up to a million times");
    }

    return tr("millions of times");
}

/*
 * Double-click handler
 */
void ReportsWidgetHibp::emitEntryActivated(const QModelIndex& index)
{
    if (!index.isValid()) {
        return;
    }

    // Find which database entry was double-clicked
    auto mappedIndex = m_modelProxy->mapToSource(index);
    const auto entry = m_rowToEntry[mappedIndex.row()];
    if (entry) {
        // Found it, invoke entry editor
        m_editedEntry = entry;
        m_editedPassword = entry->password();
        m_editedKnownBad = isKnownBad(entry);
        emit entryActivated(const_cast<Entry*>(entry));
    }
}

/*
 * Invoked after "OK" was clicked in the entry editor.
 * Re-validates the edited entry's new password.
 */
void ReportsWidgetHibp::refreshAfterEdit()
{
    // Sanity check
    if (!m_editedEntry) {
        return;
    }

    // No need to re-validate if there was no change that affects
    // the HIBP result (i. e., change to the password or to the
    // "known bad" flag)
    if (m_editedEntry->password() == m_editedPassword && isKnownBad(m_editedEntry) == m_editedKnownBad) {
        // Don't go through HIBP but still rebuild the table, the user might
        // have edited the entry title.
        makeHibpTable();
        return;
    }

    // Remove the previous password from the list of findings
    m_pwndPasswords.remove(m_editedPassword);

    // Validate the new password against HIBP
#ifdef WITH_XC_NETWORKING
    m_downloader.add(m_editedEntry->password());
    m_downloader.validate();
#endif

    m_editedEntry = nullptr;
}

void ReportsWidgetHibp::customMenuRequested(QPoint pos)
{

    // Find which entry has been clicked
    const auto index = m_ui->hibpTableView->indexAt(pos);
    if (!index.isValid()) {
        return;
    }
    auto mappedIndex = m_modelProxy->mapToSource(index);
    m_contextmenuEntry = const_cast<Entry*>(m_rowToEntry[mappedIndex.row()]);
    if (!m_contextmenuEntry) {
        return;
    }

    // Create the context menu
    const auto menu = new QMenu(this);

    // Create the "edit entry" menu item
    const auto edit = new QAction(icons()->icon("entry-edit"), tr("Edit Entry..."), this);
    menu->addAction(edit);
    connect(edit, SIGNAL(triggered()), SLOT(editFromContextmenu()));

    // Create the "exclude from reports" menu item
    const auto knownbad = new QAction(icons()->icon("reports-exclude"), tr("Exclude from reports"), this);
    knownbad->setCheckable(true);
    knownbad->setChecked(m_contextmenuEntry->customData()->contains(PasswordHealth::OPTION_KNOWN_BAD)
                         && m_contextmenuEntry->customData()->value(PasswordHealth::OPTION_KNOWN_BAD) == TRUE_STR);
    menu->addAction(knownbad);
    connect(knownbad, SIGNAL(toggled(bool)), SLOT(toggleKnownBad(bool)));

    // Show the context menu
    menu->popup(m_ui->hibpTableView->viewport()->mapToGlobal(pos));
}

void ReportsWidgetHibp::editFromContextmenu()
{
    if (m_contextmenuEntry) {
        emit entryActivated(m_contextmenuEntry);
    }
}

void ReportsWidgetHibp::toggleKnownBad(bool isKnownBad)
{
    if (!m_contextmenuEntry) {
        return;
    }

    m_contextmenuEntry->customData()->set(PasswordHealth::OPTION_KNOWN_BAD, isKnownBad ? TRUE_STR : FALSE_STR);

    makeHibpTable();
}

void ReportsWidgetHibp::saveSettings()
{
    // nothing to do - the tab is passive
}
