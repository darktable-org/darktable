#include "avifimageprovider.h"

#ifdef Q_OS_ANDROID
#  include <QJniObject>
#  include <QJniEnvironment>
#endif

AvifImageProvider::AvifImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage AvifImageProvider::requestImage(const QString &id, QSize *size,
                                        const QSize &requestedSize)
{
    // The URL is image://avif<path> where path starts with '/'.
    // Qt strips the provider name leaving the remainder; prepend '/' if needed.
    QString filePath = id;
    if (!filePath.startsWith('/'))
        filePath.prepend('/');

#ifdef Q_OS_ANDROID
    // Android BitmapFactory supports AVIF natively on API 31+ (Android 12).
    // Re-encode the decoded bitmap as JPEG and hand the bytes to QImage so we
    // avoid dealing with the bitmap pixel-format/memory details directly.
    QJniObject jPath = QJniObject::fromString(filePath);

    QJniObject bitmap = QJniObject::callStaticObjectMethod(
        "android/graphics/BitmapFactory",
        "decodeFile",
        "(Ljava/lang/String;)Landroid/graphics/Bitmap;",
        jPath.object<jstring>());

    if (!bitmap.isValid())
        return {};

    QJniObject baos("java/io/ByteArrayOutputStream", "()V");
    QJniObject jpegFmt = QJniObject::getStaticObjectField(
        "android/graphics/Bitmap$CompressFormat", "JPEG",
        "Landroid/graphics/Bitmap$CompressFormat;");
    bitmap.callMethod<jboolean>(
        "compress",
        "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z",
        jpegFmt.object(), jint(90), baos.object());

    QJniObject byteArrayObj = baos.callObjectMethod("toByteArray", "()[B");
    QJniEnvironment env;
    jbyteArray jba  = byteArrayObj.object<jbyteArray>();
    jsize      len  = env->GetArrayLength(jba);
    QByteArray data(len, '\0');
    env->GetByteArrayRegion(jba, 0, len, reinterpret_cast<jbyte *>(data.data()));

    QImage img;
    img.loadFromData(data, "JPEG");
    if (size) *size = img.size();
    if (!requestedSize.isEmpty() && !img.isNull())
        return img.scaled(requestedSize, Qt::KeepAspectRatio,
                          Qt::SmoothTransformation);
    return img;

#else
    QImage img(filePath);
    if (size) *size = img.size();
    if (!requestedSize.isEmpty() && !img.isNull())
        return img.scaled(requestedSize, Qt::KeepAspectRatio,
                          Qt::SmoothTransformation);
    return img;
#endif
}
