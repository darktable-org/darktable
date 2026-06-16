#include "daemonmanager.h"
#include <QCoreApplication>
#include <QDir>
#include <QProcessEnvironment>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_ANDROID
#  include <QtCore/qcoreapplication_platform.h>
#  include <QJniObject>
#endif

DaemonManager::DaemonManager(QObject *parent)
    : QObject(parent)
{
    QSettings s;
    m_passphrase  = s.value("p2p/passphrase").toString();
    m_staticPeers = s.value("p2p/staticPeers").toStringList();
}

DaemonManager::~DaemonManager()
{
    stop();
}

QString DaemonManager::socketPath() const
{
#ifdef Q_OS_ANDROID
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/p2p.sock";
#else
    // Reuse the socket that desktop darktable already created.
    const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR",
                                                     QDir::tempPath());
    return runtimeDir + "/darktable-p2p.sock";
#endif
}

QString DaemonManager::proxyDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/proxies";
}

QString DaemonManager::importDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/imports";
}

QString DaemonManager::passphrase() const { return m_passphrase; }

void DaemonManager::setPassphrase(const QString &passphrase)
{
    if(m_passphrase == passphrase) return;
    m_passphrase = passphrase;
    QSettings().setValue("p2p/passphrase", passphrase);
    emit passphraseChanged();
}

void DaemonManager::setStaticPeers(const QStringList &peers)
{
    m_staticPeers = peers;
    QSettings().setValue("p2p/staticPeers", peers);
    emit staticPeersChanged();
}

void DaemonManager::start()
{
    if (m_passphrase.isEmpty()) {
        setStatus("No passphrase — configure one in Settings");
        return;
    }

#ifdef Q_OS_ANDROID
    // Nothing to extract — daemon is in the JNI native-library directory.
    (void)0;
#else
    // On desktop, darktable manages its own daemon.  If the socket exists we
    // just attach; if not, we try to spawn the system-installed binary.
    if (QFileInfo::exists(socketPath())) {
        setRunning(true);
        setStatus("Attached to desktop darktable daemon");
        emit ready();
        return;
    }
#endif

    const QString exe = executablePath();
    if (!QFileInfo::exists(exe)) {
        setStatus("Daemon binary not found");
        return;
    }

    QDir().mkpath(proxyDir());
    QDir().mkpath(importDir());

    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::started,       this, &DaemonManager::onStarted);
    connect(m_proc, &QProcess::errorOccurred, this, &DaemonManager::onError);
    connect(m_proc, &QProcess::readyReadStandardError, this, &DaemonManager::onStderr);
    connect(m_proc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DaemonManager::onFinished);

    // Detect LAN IP via Qt so the Go daemon doesn't need raw netlink sockets
    // (which Android's SELinux blocks for untrusted apps).
    QString lanIP;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol
                && !entry.ip().isLoopback()) {
                lanIP = entry.ip().toString();
                break;
            }
        }
        if (!lanIP.isEmpty()) break;
    }

    QStringList args {
        "-socket",     socketPath(),
        "-passphrase", m_passphrase,
        "-proxy-dir",  proxyDir(),
        "-import-dir", importDir(),
    };
    if (!lanIP.isEmpty())
        args << "-local-ip" << lanIP;
    if (!m_staticPeers.isEmpty())
        args << "-peers" << m_staticPeers.join(',');

    m_proc->start(exe, args);
    setStatus("Starting…");
}

void DaemonManager::stop()
{
    if (m_proc) {
        m_proc->terminate();
        if (!m_proc->waitForFinished(2000))
            m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    setRunning(false);
}

void DaemonManager::restart()
{
    stop();
    QTimer::singleShot(500, this, &DaemonManager::start);
}

void DaemonManager::onStarted()
{
    // Give the daemon 1 s to bind its Unix socket before signalling ready.
    QTimer::singleShot(1000, this, [this]() {
        setRunning(true);
        setStatus("Running");
        emit ready();
    });
}

void DaemonManager::onError(QProcess::ProcessError err)
{
    setStatus(QString("Process error %1").arg(static_cast<int>(err)));
    setRunning(false);
}

void DaemonManager::onFinished(int code, QProcess::ExitStatus)
{
    setRunning(false);
    setStatus(QString("Daemon exited (%1)").arg(code));
}

void DaemonManager::onStderr()
{
    if (!m_proc) return;
    const QByteArray raw = m_proc->readAllStandardError();
    const QString text = QString::fromUtf8(raw).trimmed();
    qDebug().noquote() << "[dt-p2p-daemon]" << text;
    for (const QString &line : text.split('\n', Qt::SkipEmptyParts))
        appendLog(line.trimmed());
}

void DaemonManager::appendLog(const QString &line)
{
    if (line.isEmpty()) return;
    while (m_logLines.size() >= kMaxLogLines)
        m_logLines.removeFirst();
    m_logLines.append(line);
    emit logLinesChanged();
}

void DaemonManager::clearLog()
{
    if (!m_logLines.isEmpty()) {
        m_logLines.clear();
        emit logLinesChanged();
    }
}

QString DaemonManager::executablePath() const
{
#ifdef Q_OS_ANDROID
    // Android extracts APK native libraries to a directory that is marked
    // executable by the OS.  The daemon is packaged as libdt-p2p-daemon.so
    // (in android/libs/arm64-v8a/) so it ends up there at install time.
    //
    // QAndroidApplication::context() returns QtJniTypes::Context in Qt 6.7+.
    // QtJniTypes::JObjectBase exposes operator QJniObject(), so assigning to a
    // QJniObject gives us the full callObjectMethod / getObjectField API.
    QJniObject ctx = QNativeInterface::QAndroidApplication::context();
    QJniObject appInfo = ctx.callObjectMethod(
        "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    return appInfo.getObjectField<jstring>("nativeLibraryDir").toString()
           + "/libdt-p2p-daemon.so";
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/dt-p2p-daemon";
#endif
}

void DaemonManager::extractIfNeeded()
{
#ifndef Q_OS_ANDROID
    // On Android the daemon lives in the JNI lib dir (executable by the OS).
    // On other platforms, extract it from Qt resources if not already present.
    const QString dest = executablePath();
    if (QFileInfo::exists(dest))
        return;

    QFile src(":/assets/dt-p2p-daemon");
    if (!src.exists()) {
        qWarning("[daemon] bundled binary not in resources — skipping extract");
        return;
    }
    QDir().mkpath(QFileInfo(dest).absolutePath());
    if (src.copy(dest)) {
        QFile::setPermissions(dest,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
            QFile::ReadGroup | QFile::ExeGroup);
        qDebug("[daemon] extracted to %s", qPrintable(dest));
    } else {
        qWarning("[daemon] failed to extract binary: %s", qPrintable(src.errorString()));
    }
#endif
}

void DaemonManager::setStatus(const QString &s)
{
    if (m_status != s) { m_status = s; emit statusChanged(); }
}

void DaemonManager::setRunning(bool r)
{
    if (m_running != r) { m_running = r; emit runningChanged(); }
}
