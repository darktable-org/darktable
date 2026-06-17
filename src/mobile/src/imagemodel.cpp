#include "imagemodel.h"
#include "xmpio.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>

ImageModel::ImageModel(QObject *parent) : QAbstractListModel(parent) {}

int ImageModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
}

QVariant ImageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_entries.size()))
        return {};
    const ImageEntry &e = m_entries.at(index.row());
    switch (role) {
    case RawPathRole:         return e.rawPath;
    case ProxyPathRole:       return e.proxyPath;
    case PreviewThumbPathRole:return e.previewThumbPath;
    case FilenameRole:        return e.filename;
    case RatingRole:          return e.rating;
    case ColorLabelRole:      return e.colorLabel;
    case HasProxyRole:        return e.hasProxy;
    case PreviewKeyRole:      return e.previewKey;
    default:                  return {};
    }
}

QHash<int,QByteArray> ImageModel::roleNames() const
{
    return {
        {RawPathRole,          "rawPath"},
        {ProxyPathRole,        "proxyPath"},
        {PreviewThumbPathRole, "previewThumbPath"},
        {FilenameRole,         "filename"},
        {RatingRole,           "rating"},
        {ColorLabelRole,       "colorLabel"},
        {HasProxyRole,         "hasProxy"},
        {PreviewKeyRole,       "previewKey"},
    };
}

// ── public slots ──────────────────────────────────────────────────────────────

void ImageModel::addImage(const QString &rawPath)
{
    const QString thumbCandidate = rawPath + ".preview-thumb.jpg";
    const QString avifCandidate  = rawPath + ".proxy.avif";
    const bool thumbExists = QFileInfo::exists(thumbCandidate);
    const bool avifExists  = QFileInfo::exists(avifCandidate);

    const int existing = findByRaw(rawPath);
    if (existing >= 0) {
        // Update proxy/preview state for files that arrived since first add.
        ImageEntry &e = m_entries[existing];
        QVector<int> changed;

        if (!e.hasProxy && avifExists) {
            e.hasProxy  = true;
            e.proxyPath = avifCandidate;
            loadXmpFields(e);
            changed << HasProxyRole << ProxyPathRole << RatingRole << ColorLabelRole;
        }
        if (e.previewThumbPath.isEmpty() && thumbExists) {
            e.previewThumbPath = thumbCandidate;
            e.previewKey++;
            changed << PreviewThumbPathRole << PreviewKeyRole;
        }
        if (!changed.isEmpty())
            emit dataChanged(index(existing), index(existing), changed);

        if (e.previewThumbPath.isEmpty())
            emit previewNeeded(rawPath);
        return;
    }

    ImageEntry e;
    e.rawPath  = rawPath;
    e.filename = QFileInfo(rawPath).fileName();

    if (thumbExists) {
        e.previewThumbPath = thumbCandidate;
        e.previewKey = 1;  // thumb on disk → show immediately without waiting for preview_updated
    }
    if (avifExists) {
        e.proxyPath = avifCandidate;
        e.hasProxy  = true;
        loadXmpFields(e);
    }

    beginInsertRows({}, m_entries.size(), m_entries.size());
    m_entries.append(e);
    endInsertRows();
    emit countChanged();

    if (e.previewThumbPath.isEmpty())
        emit previewNeeded(rawPath);
    else
        checkPreviewStaleness(rawPath);
}

void ImageModel::updateProxy(const QString &rawPath, const QString &proxyPath, bool ok)
{
    const int i = findByRaw(rawPath);
    if (i < 0) return;
    m_entries[i].hasProxy  = ok;
    m_entries[i].proxyPath = ok ? proxyPath : QString{};
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {ProxyPathRole, HasProxyRole});

    if (ok && !QFileInfo::exists(rawPath + ".preview-thumb.jpg"))
        emit previewNeeded(rawPath);
}

void ImageModel::updatePreview(const QString &rawPath)
{
    const int i = findByRaw(rawPath);
    if (i < 0) return;
    m_entries[i].previewThumbPath = rawPath + ".preview-thumb.jpg";
    m_entries[i].previewKey++;
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {PreviewThumbPathRole, PreviewKeyRole});
}

void ImageModel::updateXmp(const QString &rawPath)
{
    const int i = findByRaw(rawPath);
    if (i < 0) return;
    loadXmpFields(m_entries[i]);
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {RatingRole, ColorLabelRole});
    // XMP just arrived/updated — check if the cached preview is now stale.
    checkPreviewStaleness(rawPath);
}

void ImageModel::setRating(const QString &rawPath, int rating)
{
    const int i = findByRaw(rawPath);
    if (i < 0) return;
    m_entries[i].rating = rating;
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {RatingRole});
}

void ImageModel::syncMissingPreviews()
{
    for (const ImageEntry &e : m_entries) {
        if (e.previewThumbPath.isEmpty() || !QFileInfo::exists(e.previewThumbPath))
            emit previewNeeded(e.rawPath);
    }
}

QVariantMap ImageModel::get(int index) const
{
    if (index < 0 || index >= m_entries.size())
        return {};
    const ImageEntry &e = m_entries.at(index);
    return {
        {"rawPath",          e.rawPath},
        {"proxyPath",        e.proxyPath},
        {"previewThumbPath", e.previewThumbPath},
        {"filename",         e.filename},
        {"rating",           e.rating},
        {"colorLabel",       e.colorLabel},
        {"hasProxy",         e.hasProxy},
        {"previewKey",       e.previewKey},
    };
}

QStringList ImageModel::allRawPaths() const
{
    QStringList result;
    result.reserve(m_entries.size());
    for (const ImageEntry &e : m_entries)
        result << e.rawPath;
    return result;
}

void ImageModel::setColorLabel(const QString &rawPath, int label)
{
    const int i = findByRaw(rawPath);
    if (i < 0) return;
    m_entries[i].colorLabel = label;
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {ColorLabelRole});
}

void ImageModel::scanDirectory(const QString &dir)
{
    const QDir d(dir);
    if (!d.exists()) return;

    // Collect unique raw basenames from both proxy AVIFs and JPEG previews so
    // images without a local AVIF (fetched via peer preview) are also shown.
    QSet<QString> rawNames;

    for (const QString &name : d.entryList({"*.proxy.avif"}, QDir::Files)) {
        QString raw = name;
        raw.chop(QStringLiteral(".proxy.avif").length());
        rawNames.insert(raw);
    }
    for (const QString &name : d.entryList({"*.preview-thumb.jpg"}, QDir::Files)) {
        QString raw = name;
        raw.chop(QStringLiteral(".preview-thumb.jpg").length());
        rawNames.insert(raw);
    }

    for (const QString &rawName : rawNames)
        addImage(dir + "/" + rawName);
}

// ── private ───────────────────────────────────────────────────────────────────

int ImageModel::findByRaw(const QString &rawPath) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).rawPath == rawPath)
            return i;
    return -1;
}

void ImageModel::loadXmpFields(ImageEntry &e)
{
    const QString xmpPath = e.rawPath + ".xmp";
    if (!QFileInfo::exists(xmpPath)) return;

    int rating = 0, colorLabel = -1;
    if (XmpIO::readFields(xmpPath, rating, colorLabel)) {
        e.rating     = rating;
        e.colorLabel = colorLabel;
    }
}

void ImageModel::checkPreviewStaleness(const QString &rawPath)
{
    const QString xmpPath   = rawPath + ".xmp";
    const QString thumbPath = rawPath + ".preview-thumb.jpg";
    if (!QFileInfo::exists(xmpPath) || !QFileInfo::exists(thumbPath))
        return;
    if (QFileInfo(xmpPath).lastModified() > QFileInfo(thumbPath).lastModified())
        emit previewStale(rawPath);
}
