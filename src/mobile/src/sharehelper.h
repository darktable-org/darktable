#pragma once
#include <QObject>
#include <QStringList>

// Exposed to QML as "shareHelper". Call shareRawPaths([rawPath, ...]) to open
// the system share sheet with the best available JPEG preview for each image.
class ShareHelper : public QObject
{
    Q_OBJECT
public:
    explicit ShareHelper(QObject *parent = nullptr);
    Q_INVOKABLE void shareRawPaths(const QStringList &rawPaths);
};
