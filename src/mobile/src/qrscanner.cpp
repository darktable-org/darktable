#include "qrscanner.h"

#include <QCoreApplication>
#include <QImage>
#include <QPermission>

// System-installed zxing-cpp puts headers under ZXing/; source builds (FetchContent)
// put them directly in the include root.  Support both layouts.
#if __has_include(<ZXing/ReadBarcode.h>)
#  include <ZXing/ReadBarcode.h>
#  include <ZXing/ReaderOptions.h>
#else
#  include <ReadBarcode.h>
#  include <ReaderOptions.h>
#endif

QrScanner::QrScanner(QObject *parent)
    : QObject(parent)
{}

void QrScanner::requestCameraPermission()
{
#ifdef Q_OS_ANDROID
    QCameraPermission perm;
    auto status = qApp->checkPermission(perm);
    if(status == Qt::PermissionStatus::Granted) {
        emit cameraReady();
        return;
    }
    qApp->requestPermission(perm, this, [this](const QPermission &p) {
        if(p.status() == Qt::PermissionStatus::Granted)
            emit cameraReady();
        else
            emit cameraPermissionDenied();
    });
#else
    emit cameraReady();
#endif
}

void QrScanner::attachTo(QObject *sinkObject)
{
    auto *sink = qobject_cast<QVideoSink *>(sinkObject);
    if(!sink) return;
    connect(sink, &QVideoSink::videoFrameChanged,
            this,  &QrScanner::onVideoFrame,
            Qt::DirectConnection);
}

void QrScanner::resume()
{
    m_active.store(true);
}

void QrScanner::onVideoFrame(const QVideoFrame &frame)
{
    if(!m_active.load()) return;

    QImage img = frame.toImage().convertToFormat(QImage::Format_Grayscale8);
    if(img.isNull()) return;

    ZXing::ReaderOptions opts;
    opts.setFormats(ZXing::BarcodeFormat::QRCode);
    opts.setTryHarder(false);

    ZXing::ImageView view(img.constBits(), img.width(), img.height(),
                          ZXing::ImageFormat::Lum);
    auto result = ZXing::ReadBarcode(view, opts);
    if(!result.isValid()) return;

    m_active.store(false);
    emit qrDecoded(QString::fromStdString(result.text()));
}
