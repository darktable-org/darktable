#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QStringList>

// Talks to dt-p2p-daemon over its Unix domain socket (JSON-lines protocol).
//
// Two connection patterns are used:
//   subscribe_events  — one persistent socket; daemon pushes events back.
//   commands          — a fresh socket per command; fire-and-forget or
//                       single-response (fetch_proxy).
class P2PClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool        connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(QStringList peers     READ peers       NOTIFY peersChanged)

public:
    explicit P2PClient(QObject *parent = nullptr);

    bool        isConnected() const { return m_connected; }
    QStringList peers()       const { return m_peers;     }

    void setSocketPath(const QString &path);

public slots:
    void connectToDaemon();
    void disconnectFromDaemon();

    // Fire-and-forget commands
    void pushXmp(const QString &rawPath, const QString &xmpContent);
    // Write rating + colorLabel into the local .xmp sidecar and push to peers.
    Q_INVOKABLE void applyAndPushEdits(const QString &rawPath, int rating, int colorLabel);
    void announceProxy(const QString &rawPath);
    void fetchPreview(const QString &rawPath, const QString &size = QLatin1String("thumb"));
    // Ask daemon to re-sync with all known peers (discover new images, refresh previews).
    void requestSync();
    // Delete local preview cache and re-download from peer (used when XMP is newer than JPEG).
    void forceRefreshPreview(const QString &rawPath);

    // Commands with a response — result arrives via signal
    void fetchProxy(const QString &rawPath);
    void listPeers();
    void ping();

signals:
    void connectedChanged();
    void peersChanged();
    void imageImported(const QString &path);
    void proxyFetched(const QString &rawPath, const QString &proxyPath, bool ok);
    void xmpUpdated(const QString &path);
    void previewUpdated(const QString &rawPath);

private slots:
    void onEventConnected();
    void onEventDisconnected();
    void onEventReadyRead();
    void onEventError(QLocalSocket::LocalSocketError err);

private:
    void dispatchEvent(const QJsonObject &msg);
    void sendFireAndForget(const QJsonObject &cmd);
    void sendWithResponse(const QJsonObject &cmd,
                          std::function<void(const QJsonObject &)> callback);
    void setConnected(bool c);

    QLocalSocket *m_eventSocket = nullptr;
    QString       m_socketPath;
    bool          m_connected = false;
    QStringList   m_peers;
    QByteArray    m_readBuf;
    // Epoch-ms timestamp of the last xmp_updated received per raw path.
    // Used by applyAndPushEdits to suppress echoing back a peer's edit.
    QHash<QString, qint64> m_xmpReceivedAt;
};
