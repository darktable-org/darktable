#pragma once
#include <QQuickImageProvider>

// Image provider registered as "image://avif/<absolute-path>".
// On Android it delegates to BitmapFactory (supports AVIF, JPEG, WebP, PNG
// natively; AVIF requires API 31+).  On other platforms falls back to QImage.
class AvifImageProvider : public QQuickImageProvider
{
public:
    AvifImageProvider();
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
