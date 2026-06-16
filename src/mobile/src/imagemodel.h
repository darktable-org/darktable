#pragma once
#include <QAbstractListModel>
#include <QList>
#include <QString>

struct ImageEntry
{
    QString rawPath;      // canonical path used for XMP push
    QString proxyPath;    // local .proxy.avif file path
    QString filename;     // display name (basename of rawPath)
    int     rating     = 0;   // 0–5
    int     colorLabel = -1;  // -1=none  0=red 1=yellow 2=green 3=blue 4=purple
    bool    hasProxy   = false;
};

class ImageModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        RawPathRole = Qt::UserRole + 1,
        ProxyPathRole,
        FilenameRole,
        RatingRole,
        ColorLabelRole,
        HasProxyRole,
    };

    explicit ImageModel(QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int,QByteArray> roleNames() const override;

    Q_INVOKABLE void setRating(const QString &rawPath, int rating);
    Q_INVOKABLE void setColorLabel(const QString &rawPath, int label);

    // Scan an existing directory for *.proxy.avif files on startup
    void scanDirectory(const QString &dir);

public slots:
    void addImage(const QString &rawPath);
    void updateProxy(const QString &rawPath, const QString &proxyPath, bool ok);
    void updateXmp(const QString &rawPath);

signals:
    void countChanged();

private:
    int findByRaw(const QString &rawPath) const;
    void loadXmpFields(ImageEntry &e);

    QList<ImageEntry> m_entries;
};
