#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "daemonmanager.h"
#include "imagemodel.h"
#include "p2pclient.h"

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

    // When a new image arrives, ensure we have a proxy for it.
    QObject::connect(&client, &P2PClient::imageImported,
                     &model,  &ImageModel::addImage);
    QObject::connect(&client, &P2PClient::imageImported, &client,
                     [&client](const QString &rawPath) {
                         // Request a proxy if one wasn't auto-fetched already.
                         client.fetchProxy(rawPath);
                     });
    QObject::connect(&client, &P2PClient::proxyFetched, &model,
                     [&model](const QString &rawPath, const QString &proxyPath, bool ok) {
                         model.updateProxy(rawPath, proxyPath, ok);
                     });
    QObject::connect(&client, &P2PClient::xmpUpdated, &model, &ImageModel::updateXmp);

    // Start daemon then connect the client socket.
    QObject::connect(&daemon, &DaemonManager::ready, &client,
                     [&daemon, &client, &model]() {
                         client.setSocketPath(daemon.socketPath());
                         client.connectToDaemon();
                         model.scanDirectory(daemon.importDir());
                     });

    QQmlApplicationEngine engine;
    auto *ctx = engine.rootContext();
    ctx->setContextProperty("daemon",     &daemon);
    ctx->setContextProperty("p2p",        &client);
    ctx->setContextProperty("imageModel", &model);

    // Qt 6.4+: loadFromModule resolves via the QML module system, avoiding
    // any dependency on the internal resource prefix (/qt/qml/ in Qt 6.5+).
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("DarktableMobile", "Main");

    daemon.start();
    return app.exec();
}
