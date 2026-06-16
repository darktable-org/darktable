#include "imagemodel.h"
#include "xmpio.h"
#include <QDir>
#include <QFileInfo>

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
    case RawPathRole:    return e.rawPath;
    case ProxyPathRole:  return e.proxyPath;
    case FilenameRole:   return e.filename;
    case RatingRole:     return e.rating;
    case ColorLabelRole: return e.colorLabel;
    case HasProxyRole:   return e.hasProxy;
    default:             return {};
    }
}

QHash<int,QByteArray> ImageModel::roleNames() const
{
    return {
        {RawPathRole,    "rawPath"},
        {ProxyPathRole,  "proxyPath"},
        {FilenameRole,   "filename"},
        {RatingRole,     "rating"},
        {ColorLabelRole, "colorLabel"},
        {HasProxyRole,   "hasProxy"},
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

    // Check if a proxy already exists beside the raw or in the same dir
    const QString candidate = rawPath + ".proxy.avif";
    if (QFileInfo::exists(candidate)) {
        e.proxyPath = candidate;
        e.hasProxy  = true;
        loadXmpFields(e);
    }

    beginInsertRows({}, m_entries.size(), m_entries.size());
    m_entries.append(e);
    endInsertRows();
    emit countChanged();
}

void ImageModel::updateProxy(const QString &rawPath, const QString &proxyPath, bool ok)
{
    const int i = findByRaw(rawPath);
    if (i < 0) return;
    m_entries[i].hasProxy  = ok;
    m_entries[i].proxyPath = ok ? proxyPath : QString{};
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {ProxyPathRole, HasProxyRole});
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

    const QStringList proxies = d.entryList({"*.proxy.avif"}, QDir::Files);
    for (const QString &name : proxies) {
        // Strip ".proxy.avif" to recover the raw filename
        QString rawName = name;
        rawName.chop(QStringLiteral(".proxy.avif").length());
        const QString rawPath = dir + "/" + rawName;
        addImage(rawPath);
    }
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
