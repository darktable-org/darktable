#pragma once

#include <QObject>
#include <QString>
#include <QVideoSink>
#include <QVideoFrame>
#include <atomic>

// Decodes QR codes from camera frames delivered by a QVideoSink.
//
// Typical QML usage:
//   Component.onCompleted: qrScanner.requestCameraPermission()
//   Connections { target: qrScanner; function onCameraReady() {
//       cam.active = true
//       qrScanner.attachTo(viewfinder.videoSink)
//   }}
//
// When a QR code is found, qrDecoded(text) is emitted once.
// Call resume() to re-enable scanning after handling the result.
class QrScanner : public QObject
{
    Q_OBJECT

public:
    explicit QrScanner(QObject *parent = nullptr);

    // Request CAMERA permission on Android, then emit cameraReady() or
    // cameraPermissionDenied().  On non-Android platforms, emits cameraReady()
    // immediately (permission is not required).
    Q_INVOKABLE void requestCameraPermission();

    // Connect to a VideoOutput's internal sink to receive camera frames.
    Q_INVOKABLE void attachTo(QObject *sinkObject);

    // Re-enable scanning after qrDecoded was handled.
    Q_INVOKABLE void resume();

signals:
    void cameraReady();
    void cameraPermissionDenied();
    void qrDecoded(const QString &text);

private slots:
    void onVideoFrame(const QVideoFrame &frame);

private:
    std::atomic<bool> m_active{true};
};
