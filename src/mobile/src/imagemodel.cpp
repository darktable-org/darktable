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
    if (findByRaw(rawPath) >= 0)
        return;  // already known

    ImageEntry e;
    e.rawPath  = rawPath;
    e.filename = QFileInfo(rawPath).fileName();

    // Prefer an existing JPEG thumb preview for display; fall back to AVIF proxy.
    const QString thumbCandidate = rawPath + ".preview-thumb.jpg";
    if (QFileInfo::exists(thumbCandidate))
        e.previewThumbPath = thumbCandidate;

    const QString avifCandidate = rawPath + ".proxy.avif";
    if (QFileInfo::exists(avifCandidate)) {
        e.proxyPath = avifCandidate;
        e.hasProxy  = true;
        loadXmpFields(e);
    }

    beginInsertRows({}, m_entries.size(), m_entries.size());
    m_entries.append(e);
    endInsertRows();
    emit countChanged();

    // Always request a thumbnail when we don't have one locally — the daemon
    // can fetch it from a peer even if the full AVIF isn't downloaded yet.
    if (e.previewThumbPath.isEmpty())
        emit previewNeeded(rawPath);
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
