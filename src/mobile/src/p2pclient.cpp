#include "p2pclient.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <functional>

P2PClient::P2PClient(QObject *parent) : QObject(parent) {}

void P2PClient::setSocketPath(const QString &path) { m_socketPath = path; }

// ── event subscription ────────────────────────────────────────────────────────

void P2PClient::connectToDaemon()
{
    if (m_eventSocket)
        return;

    m_eventSocket = new QLocalSocket(this);
    connect(m_eventSocket, &QLocalSocket::connected,
            this, &P2PClient::onEventConnected);
    connect(m_eventSocket, &QLocalSocket::disconnected,
            this, &P2PClient::onEventDisconnected);
    connect(m_eventSocket, &QLocalSocket::readyRead,
            this, &P2PClient::onEventReadyRead);
    connect(m_eventSocket, &QLocalSocket::errorOccurred,
            this, &P2PClient::onEventError);
    m_eventSocket->connectToServer(m_socketPath);
}

void P2PClient::disconnectFromDaemon()
{
    if (m_eventSocket) {
        m_eventSocket->disconnectFromServer();
        m_eventSocket->deleteLater();
        m_eventSocket = nullptr;
    }
    setConnected(false);
}

void P2PClient::onEventConnected()
{
    setConnected(true);
    const QJsonObject sub { {"type", "subscribe_events"} };
    m_eventSocket->write(QJsonDocument(sub).toJson(QJsonDocument::Compact) + "\n");
    listPeers();
}

void P2PClient::onEventDisconnected() { setConnected(false); }

void P2PClient::onEventReadyRead()
{
    m_readBuf += m_eventSocket->readAll();
    int nl;
    while ((nl = m_readBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_readBuf.left(nl).trimmed();
        m_readBuf.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        const QJsonObject msg = QJsonDocument::fromJson(line).object();
        if (!msg.isEmpty())
            dispatchEvent(msg);
    }
}

void P2PClient::onEventError(QLocalSocket::LocalSocketError err)
{
    qWarning("[p2p] event socket error %d", static_cast<int>(err));
    setConnected(false);
}

void P2PClient::dispatchEvent(const QJsonObject &msg)
{
    const QString   type = msg["type"].toString();
    const QJsonObject d  = QJsonDocument::fromJson(
        QJsonDocument(msg["data"].toObject()).toJson()).object();

    if      (type == "xmp_updated")    emit xmpUpdated(d["path"].toString());
    else if (type == "image_imported") emit imageImported(d["path"].toString());
    else if (type == "peers") {
        QStringList list;
        for (const auto &v : msg["data"].toArray())
            list << v.toString();
        if (m_peers != list) { m_peers = list; emit peersChanged(); }
    }
}

// ── helpers ───────────────────────────────────────────────────────────────────

void P2PClient::sendFireAndForget(const QJsonObject &cmd)
{
    auto *sock = new QLocalSocket(this);
    const QByteArray line = QJsonDocument(cmd).toJson(QJsonDocument::Compact) + "\n";
    connect(sock, &QLocalSocket::connected, sock, [sock, line]() {
        sock->write(line);
        sock->flush();
        QTimer::singleShot(300, sock, &QLocalSocket::deleteLater);
    });
    connect(sock, &QLocalSocket::errorOccurred, sock, &QLocalSocket::deleteLater);
    sock->connectToServer(m_socketPath);
}

void P2PClient::sendWithResponse(const QJsonObject &cmd,
                                 std::function<void(const QJsonObject &)> callback)
{
    auto *sock = new QLocalSocket(this);
    auto *buf  = new QByteArray;

    const QByteArray line = QJsonDocument(cmd).toJson(QJsonDocument::Compact) + "\n";

    connect(sock, &QLocalSocket::connected, sock, [sock, line]() {
        sock->write(line);
        sock->flush();
    });
    connect(sock, &QLocalSocket::readyRead, sock, [sock, buf, callback]() {
        *buf += sock->readAll();
        int nl;
        while ((nl = buf->indexOf('\n')) >= 0) {
            const QByteArray msgLine = buf->left(nl).trimmed();
            buf->remove(0, nl + 1);
            if (msgLine.isEmpty()) continue;
            const QJsonObject resp = QJsonDocument::fromJson(msgLine).object();
            if (!resp.isEmpty()) {
                callback(resp);
                sock->disconnectFromServer();
                return;
            }
        }
    });
    connect(sock, &QLocalSocket::disconnected, sock, [sock, buf]() {
        delete buf;
        sock->deleteLater();
    });
    connect(sock, &QLocalSocket::errorOccurred, sock, [sock, buf]() {
        delete buf;
        sock->deleteLater();
    });

    sock->connectToServer(m_socketPath);
}

// ── public commands ───────────────────────────────────────────────────────────

void P2PClient::pushXmp(const QString &rawPath, const QString &xmpContent)
{
    sendFireAndForget({
        {"type", "xmp_push"},
        {"data", QJsonObject{
            {"path",    rawPath},
            {"content", xmpContent},
            {"mtime",   QString::number(QDateTime::currentMSecsSinceEpoch() * 1'000'000LL)},
        }}
    });
}

void P2PClient::announceProxy(const QString &rawPath)
{
    sendFireAndForget({
        {"type", "announce_proxy"},
        {"data", QJsonObject{{"path", rawPath}}}
    });
}

void P2PClient::fetchProxy(const QString &rawPath)
{
    sendWithResponse(
        {{"type", "fetch_proxy"}, {"data", QJsonObject{{"path", rawPath}}}},
        [this, rawPath](const QJsonObject &resp) {
            if (resp["type"].toString() != "proxy_fetched") return;
            const QJsonObject d = QJsonDocument::fromJson(
                QJsonDocument(resp["data"].toObject()).toJson()).object();
            const bool ok = d["status"].toString() == "ok";
            emit proxyFetched(rawPath, d["path"].toString(), ok);
        });
}

void P2PClient::listPeers()
{
    sendWithResponse(
        {{"type", "list_peers"}},
        [this](const QJsonObject &resp) {
            if (resp["type"].toString() != "peers") return;
            QStringList list;
            for (const auto &v : resp["data"].toArray())
                list << v.toString();
            if (m_peers != list) { m_peers = list; emit peersChanged(); }
        });
}

void P2PClient::ping()
{
    sendWithResponse({{"type", "ping"}}, [](const QJsonObject &) {});
}

void P2PClient::setConnected(bool c)
{
    if (m_connected != c) { m_connected = c; emit connectedChanged(); }
}
