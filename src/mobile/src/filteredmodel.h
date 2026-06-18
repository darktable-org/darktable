#pragma once
#include <QSortFilterProxyModel>
#include <QStringList>

// Filtering and sorting proxy for ImageModel.
// filmRoll: "" = all; otherwise match the YYYYMMDD date prefix of the filename.
// minRating: 0 = all; N = show only entries with rating >= N.
// colorLabel: -2 = all; -1 = unlabeled only; 0-4 = specific color.
// sortByDate: true = newest-date-prefix first, then filename; false = filename A-Z.
class FilterSortModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString     filmRoll   READ filmRoll   WRITE setFilmRoll   NOTIFY filmRollChanged)
    Q_PROPERTY(int         minRating  READ minRating  WRITE setMinRating  NOTIFY minRatingChanged)
    Q_PROPERTY(int         colorLabel READ colorLabel WRITE setColorLabel NOTIFY colorLabelChanged)
    Q_PROPERTY(bool        sortByDate READ sortByDate WRITE setSortByDate NOTIFY sortByDateChanged)
    Q_PROPERTY(QStringList filmRolls  READ filmRolls                      NOTIFY filmRollsChanged)
    Q_PROPERTY(int         count      READ count                          NOTIFY countChanged)

public:
    explicit FilterSortModel(QObject *parent = nullptr);
    void setSourceModel(QAbstractItemModel *m) override;

    QString     filmRoll()   const { return m_filmRoll; }
    int         minRating()  const { return m_minRating; }
    int         colorLabel() const { return m_colorLabel; }
    bool        sortByDate() const { return m_sortByDate; }
    QStringList filmRolls()  const;
    int         count()      const { return rowCount(); }

    void setFilmRoll(const QString &v);
    void setMinRating(int v);
    void setColorLabel(int v);
    void setSortByDate(bool v);

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QStringList allRawPaths() const;
    // True if any filter is currently active.
    Q_INVOKABLE bool isFiltered() const;

signals:
    void filmRollChanged();
    void minRatingChanged();
    void colorLabelChanged();
    void sortByDateChanged();
    void filmRollsChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    static QString filmRollFromFilename(const QString &filename);

    QString m_filmRoll;
    int     m_minRating  = 0;
    int     m_colorLabel = -2;
    bool    m_sortByDate = true;
};
