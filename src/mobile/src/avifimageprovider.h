#pragma once
#include <QQuickImageProvider>

// Image provider registered as "image://avif/<raw-path>?k=<cache-key>".
// Serves JPEG previews fetched from the desktop peer (instant, no codec).
// On Android: returns null if no JPEG exists — show placeholder until the
// daemon delivers one via the preview_updated event.
// On desktop: falls back to Qt's native AVIF plugin (raw + ".proxy.avif").
class AvifImageProvider : public QQuickImageProvider
{
public:
    AvifImageProvider();
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
