#include "pairingmanager.h"
#include "daemonmanager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>

PairingManager::PairingManager(DaemonManager *daemon, QObject *parent)
    : QObject(parent), m_daemon(daemon)
{}

bool PairingManager::parseUrl(const QString &url)
{
    // Expected: darktable://pair?d=BASE64_URL_SAFE_JSON
    QUrl u(url);
    if(u.scheme() != QLatin1String("darktable") || u.host() != QLatin1String("pair"))
        return false;

    QString encoded = QUrlQuery(u).queryItemValue(QStringLiteral("d"));
    if(encoded.isEmpty())
        return false;

    // Restore standard base64 padding and characters.
    QString b64 = encoded;
    b64.replace(QLatin1Char('-'), QLatin1Char('+'));
    b64.replace(QLatin1Char('_'), QLatin1Char('/'));
    while(b64.size() % 4) b64.append(QLatin1Char('='));

    QByteArray json = QByteArray::fromBase64(b64.toLatin1());
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if(err.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    QJsonObject obj = doc.object();
    if(obj.value(QStringLiteral("v")).toInt() != 1)
        return false;

    m_passphrase  = obj.value(QStringLiteral("pp")).toString();
    m_fingerprint = obj.value(QStringLiteral("fpr")).toString();
    m_peers.clear();
    for(const auto &v : obj.value(QStringLiteral("peers")).toArray())
        m_peers.append(v.toString());

    if(m_passphrase.isEmpty())
        return false;

    m_hasPending = true;
    emit pendingChanged();
    return true;
}

void PairingManager::accept()
{
    if(!m_hasPending) return;

    m_daemon->setPassphrase(m_passphrase);
    if(!m_peers.isEmpty())
        m_daemon->setStaticPeers(m_peers);

    m_daemon->restart();
    reject();
    emit pairingAccepted();
}

void PairingManager::reject()
{
    m_passphrase.clear();
    m_fingerprint.clear();
    m_peers.clear();
    m_hasPending = false;
    emit pendingChanged();
}
