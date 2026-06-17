#include "avifimageprovider.h"

#include <QImage>
#include <QDebug>

// Image provider registered as "image://avif/<raw-path>?k=<cache-key>".
//
// On Android: displays ONLY from JPEG previews fetched from peers via the
// p2p daemon (/preview endpoint). No AVIF / MediaCodec decoding is attempted.
// If no JPEG preview exists yet, returns a null image so QML shows a
// placeholder while the daemon fetches it in the background.
//
// On desktop: falls back to Qt's native AVIF plugin (requires Qt ImageFormats
// or libavif) when no peer-fetched JPEG preview is available.

AvifImageProvider::AvifImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage AvifImageProvider::requestImage(const QString &id, QSize *size,
                                        const QSize &requestedSize)
{
    QString filePath = id;
    if (!filePath.startsWith('/'))
        filePath.prepend('/');

    // Strip the cache-busting query string appended by QML (?k=N).
    const int q = filePath.indexOf('?');
    if (q >= 0)
        filePath.truncate(q);

    // Normalise to the canonical raw path (strip legacy ".proxy.avif" suffix).
    QString rawBase = filePath;
    if (rawBase.endsWith(QLatin1String(".proxy.avif")))
        rawBase.chop(11);

    // sourceSize.width: 400 in QML → Qt passes QSize(400, -1); height is -1
    // when the dimension is unconstrained. QSize::isValid() requires both
    // dimensions ≥ 0, so check only the width to determine thumbnail intent.
    // width ≤ 0 means no sourceSize hint at all → full-screen context.
    const bool wantFull = requestedSize.width() <= 0
                       || requestedSize.width() > 480;

    // Prefer the size that matches the context; fall back to whatever exists.
    const QString primaryPath = rawBase
        + (wantFull ? QLatin1String(".preview-full.jpg")
                    : QLatin1String(".preview-thumb.jpg"));
    const QString fallbackPath = rawBase
        + (wantFull ? QLatin1String(".preview-thumb.jpg")
                    : QLatin1String(".preview-full.jpg"));

    QImage preview(primaryPath);
    if (preview.isNull())
        preview = QImage(fallbackPath);

    if (!preview.isNull()) {
        if (size) *size = preview.size();
        if (requestedSize.width() > 0 && requestedSize.height() > 0)
            return preview.scaled(requestedSize, Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation);
        return preview;
    }

#ifdef Q_OS_ANDROID
    // Mobile display is exclusively from peer-fetched JPEG previews.
    // Return nothing until one arrives; the previewNeeded → fetchPreview
    // chain running in the daemon will deliver it via preview_updated.
    return {};
#else
    // Desktop fallback: load the proxy AVIF via Qt's native AVIF plugin.
    QImage img(rawBase + QLatin1String(".proxy.avif"));
    if (img.isNull())
        qWarning() << "AvifProvider: cannot load proxy for" << rawBase;
    if (size) *size = img.size();
    if (!requestedSize.isEmpty() && !img.isNull())
        return img.scaled(requestedSize, Qt::KeepAspectRatio,
                          Qt::SmoothTransformation);
    return img;
#endif
}
