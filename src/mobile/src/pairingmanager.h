#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class DaemonManager;

// Parses a darktable://pair?d=BASE64 URL decoded from a QR code and applies
// the embedded settings (passphrase + peers) to the daemon after user confirmation.
class PairingManager : public QObject
{
    Q_OBJECT

public:
    explicit PairingManager(DaemonManager *daemon, QObject *parent = nullptr);

    // Parse and validate a scanned URL.  Returns true if the URL is a valid
    // darktable pairing URL and pendingXxx properties are populated.
    Q_INVOKABLE bool parseUrl(const QString &url);

    // Apply the pending pairing settings to the daemon and restart it.
    Q_INVOKABLE void accept();

    // Clear pending state without applying anything.
    Q_INVOKABLE void reject();

    Q_PROPERTY(bool     hasPending   READ hasPending   NOTIFY pendingChanged)
    Q_PROPERTY(QString  pendingPeers READ pendingPeers NOTIFY pendingChanged)

    bool    hasPending()   const { return m_hasPending; }
    QString pendingPeers() const { return m_peers.join(", "); }

signals:
    void pendingChanged();
    void pairingAccepted();

private:
    DaemonManager *m_daemon;
    bool           m_hasPending = false;
    QString        m_passphrase;
    QString        m_fingerprint;
    QStringList    m_peers;
};
