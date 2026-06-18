#pragma once
#include <QAbstractListModel>
#include <QList>
#include <QString>

struct ImageEntry
{
    QString rawPath;          // canonical path used for XMP push
    QString proxyPath;        // local .proxy.avif file path
    QString previewThumbPath; // local .preview-thumb.jpg path (preferred for display)
    QString filename;         // display name (basename of rawPath)
    int     rating     = 0;   // 0–5
    int     colorLabel = -1;  // -1=none  0=red 1=yellow 2=green 3=blue 4=purple
    bool    hasProxy        = false;
    int     previewKey      = 0;     // bumped when a cached preview JPEG is refreshed
    bool    previewIsStale  = false; // XMP newer than preview; fresh fetch pending
};

class ImageModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        RawPathRole = Qt::UserRole + 1,
        ProxyPathRole,
        PreviewThumbPathRole,
        FilenameRole,
        RatingRole,
        ColorLabelRole,
        HasProxyRole,
        PreviewKeyRole,
    };

    explicit ImageModel(QObject *parent = nullptr);

    int      rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int,QByteArray> roleNames() const override;

    Q_INVOKABLE void setRating(const QString &rawPath, int rating);
    Q_INVOKABLE void setColorLabel(const QString &rawPath, int label);
    Q_INVOKABLE QStringList allRawPaths() const;
    // Return all fields for the entry at index as a QVariantMap (for QML darkroom navigation).
    Q_INVOKABLE QVariantMap get(int index) const;

    // Scan an existing directory for *.proxy.avif files on startup
    void scanDirectory(const QString &dir);

public slots:
    void addImage(const QString &rawPath);
    void updateProxy(const QString &rawPath, const QString &proxyPath, bool ok);
    void updateXmp(const QString &rawPath);
    void updatePreview(const QString &rawPath);
    // Mark this entry's preview as stale so syncMissingPreviews will retry it.
    void markPreviewStale(const QString &rawPath);
    // Emit previewNeeded for entries with no thumbnail, and previewStale for
    // entries whose preview is stale.  Call periodically to recover from failures.
    void syncMissingPreviews();

signals:
    void countChanged();
    void previewNeeded(const QString &rawPath);
    // Emitted when the .xmp sidecar is newer than the cached preview JPEG,
    // meaning the desktop has unapplied edits that should trigger a re-render.
    void previewStale(const QString &rawPath);

private:
    int findByRaw(const QString &rawPath) const;
    void loadXmpFields(ImageEntry &e);
    void checkPreviewStaleness(const QString &rawPath);

    QList<ImageEntry> m_entries;
};
