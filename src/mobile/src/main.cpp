#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>

#include "avifimageprovider.h"
#include "daemonmanager.h"
#include "imagemodel.h"
#include "p2pclient.h"
#include "pairingmanager.h"
#include "qrscanner.h"
#include "sharehelper.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("Darktable Mobile");
    app.setOrganizationName("darktable");
    app.setOrganizationDomain("net.darktable.mobile");

    QQuickStyle::setStyle("Material");

    DaemonManager daemon;
    P2PClient     client;
    ImageModel    model;
    PairingManager pairing(&daemon);
    QrScanner     qrScanner;
    ShareHelper   shareHelper;

    // When a new image arrives, ensure we have a proxy for it.
    QObject::connect(&client, &P2PClient::imageImported,
                     &model,  &ImageModel::addImage);
    QObject::connect(&client, &P2PClient::imageImported, &client,
                     [&client](const QString &rawPath) {
                         client.fetchProxy(rawPath);
                     });
    QObject::connect(&client, &P2PClient::proxyFetched, &model,
                     [&model](const QString &rawPath, const QString &proxyPath, bool ok) {
                         model.updateProxy(rawPath, proxyPath, ok);
                     });
    QObject::connect(&client, &P2PClient::xmpUpdated,      &model,  &ImageModel::updateXmp);
    QObject::connect(&client, &P2PClient::previewUpdated,  &model,  &ImageModel::updatePreview);

    // When an image has a proxy but no local preview JPEG, request one from peers.
    QObject::connect(&model,  &ImageModel::previewNeeded,  &client,
                     [&client](const QString &rawPath) {
                         client.fetchPreview(rawPath, QStringLiteral("thumb"));
                     });

    // Start daemon then connect the client socket.
    QObject::connect(&daemon, &DaemonManager::ready, &client,
                     [&daemon, &client, &model]() {
                         client.setSocketPath(daemon.socketPath());
                         client.connectToDaemon();
                         model.scanDirectory(daemon.importDir());
                         // Trigger an immediate peer sync so images appear without
                         // waiting for the first gossip announce or manifest poll.
                         QTimer::singleShot(3000, &client, [&client]() {
                             client.requestSync();
                         });
                     });

    // Periodic background sync: every 45 s ask the daemon to re-check peers for
    // new images and fresher previews, then scan the model for any entries that
    // still have no local thumbnail and request them.
    auto *syncTimer = new QTimer(&app);
    syncTimer->setInterval(45 * 1000);
    QObject::connect(syncTimer, &QTimer::timeout, &client, [&client, &model]() {
        client.requestSync();
        model.syncMissingPreviews();
    });
    QObject::connect(&client, &P2PClient::connectedChanged, syncTimer,
                     [&client, syncTimer]() {
                         if (client.isConnected()) syncTimer->start();
                         else syncTimer->stop();
                     });

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("avif"), new AvifImageProvider);
    auto *ctx = engine.rootContext();
    ctx->setContextProperty("daemon",     &daemon);
    ctx->setContextProperty("p2p",        &client);
    ctx->setContextProperty("imageModel", &model);
    ctx->setContextProperty("pairing",     &pairing);
    ctx->setContextProperty("qrScanner",  &qrScanner);
    ctx->setContextProperty("shareHelper",&shareHelper);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("DarktableMobile", "Main");

    daemon.start();
    return app.exec();
}
