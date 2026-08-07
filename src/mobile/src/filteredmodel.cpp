#include "filteredmodel.h"
#include "imagemodel.h"
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

FilterSortModel::FilterSortModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    sort(0);
}

void FilterSortModel::setSourceModel(QAbstractItemModel *m)
{
    QSortFilterProxyModel::setSourceModel(m);
    if (m) {
        connect(m, &QAbstractItemModel::rowsInserted,
                this, &FilterSortModel::filmRollsChanged);
        connect(m, &QAbstractItemModel::rowsRemoved,
                this, &FilterSortModel::filmRollsChanged);
        connect(m, &QAbstractItemModel::modelReset,
                this, &FilterSortModel::filmRollsChanged);
        connect(m, &QAbstractItemModel::dataChanged,
                this, &FilterSortModel::filmRollsChanged);
    }
    connect(this, &QAbstractItemModel::rowsInserted,
            this, &FilterSortModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved,
            this, &FilterSortModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset,
            this, &FilterSortModel::countChanged);
}

// Extract YYYYMMDD prefix from filenames like "20260615_0001.CR3".
// Returns "" for filenames without that pattern.
QString FilterSortModel::filmRollFromFilename(const QString &filename)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{8})_"));
    const auto m = re.match(filename);
    return m.hasMatch() ? m.captured(1) : QString{};
}

QStringList FilterSortModel::filmRolls() const
{
    const QAbstractItemModel *src = sourceModel();
    if (!src) return {};
    QSet<QString> seen;
    QStringList result;
    for (int i = 0; i < src->rowCount(); ++i) {
        const QString fn   = src->data(src->index(i, 0), ImageModel::FilenameRole).toString();
        const QString roll = filmRollFromFilename(fn);
        if (!roll.isEmpty() && !seen.contains(roll)) {
            seen.insert(roll);
            result.append(roll);
        }
    }
    // Newest first.
    std::sort(result.begin(), result.end(), std::greater<QString>());
    return result;
}

void FilterSortModel::setFilmRoll(const QString &v)
{
    if (m_filmRoll == v) return;
    m_filmRoll = v;
    invalidateFilter();
    emit filmRollChanged();
    emit countChanged();
}

void FilterSortModel::setMinRating(int v)
{
    if (m_minRating == v) return;
    m_minRating = v;
    invalidateFilter();
    emit minRatingChanged();
    emit countChanged();
}

void FilterSortModel::setColorLabel(int v)
{
    if (m_colorLabel == v) return;
    m_colorLabel = v;
    invalidateFilter();
    emit colorLabelChanged();
    emit countChanged();
}

void FilterSortModel::setSortByDate(bool v)
{
    if (m_sortByDate == v) return;
    m_sortByDate = v;
    invalidate(); // re-sort and re-filter
    emit sortByDateChanged();
}

bool FilterSortModel::filterAcceptsRow(int sourceRow, const QModelIndex &) const
{
    const QAbstractItemModel *src = sourceModel();
    if (!src) return false;
    const QModelIndex idx = src->index(sourceRow, 0);

    if (!m_filmRoll.isEmpty()) {
        const QString fn = src->data(idx, ImageModel::FilenameRole).toString();
        if (filmRollFromFilename(fn) != m_filmRoll)
            return false;
    }
    if (m_minRating > 0) {
        if (src->data(idx, ImageModel::RatingRole).toInt() < m_minRating)
            return false;
    }
    if (m_colorLabel >= -1) {
        if (src->data(idx, ImageModel::ColorLabelRole).toInt() != m_colorLabel)
            return false;
    }
    return true;
}

bool FilterSortModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const QAbstractItemModel *src = sourceModel();
    if (!src) return false;
    const QString lname = src->data(left,  ImageModel::FilenameRole).toString();
    const QString rname = src->data(right, ImageModel::FilenameRole).toString();
    if (m_sortByDate) {
        const QString lroll = filmRollFromFilename(lname);
        const QString rroll = filmRollFromFilename(rname);
        if (lroll != rroll)
            return lroll > rroll; // newest date first
    }
    return lname.compare(rname, Qt::CaseInsensitive) < 0;
}

QVariantMap FilterSortModel::get(int row) const
{
    if (row < 0 || row >= rowCount()) return {};
    const QModelIndex src = mapToSource(index(row, 0));
    if (!src.isValid()) return {};
    return static_cast<const ImageModel *>(sourceModel())->get(src.row());
}

QStringList FilterSortModel::allRawPaths() const
{
    QStringList result;
    result.reserve(rowCount());
    for (int i = 0; i < rowCount(); ++i)
        result << data(index(i, 0), ImageModel::RawPathRole).toString();
    return result;
}

bool FilterSortModel::isFiltered() const
{
    return !m_filmRoll.isEmpty() || m_minRating > 0 || m_colorLabel >= -1;
}
