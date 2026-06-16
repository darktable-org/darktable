#pragma once
#include <QObject>
#include <QProcess>
#include <QString>

// Manages the lifecycle of the dt-p2p-daemon subprocess.
// On Android the binary is extracted from Qt resources to the app data dir
// and chmod'd executable before the first launch.
class DaemonManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QString status  READ status    NOTIFY statusChanged)

public:
    explicit DaemonManager(QObject *parent = nullptr);
    ~DaemonManager();

    bool    isRunning() const { return m_running; }
    QString status()    const { return m_status;  }

    QString socketPath()  const;
    QString proxyDir()    const;
    QString importDir()   const;
    QString passphrase()  const;

public slots:
    void start();
    void stop();
    void restart();
    void setPassphrase(const QString &passphrase);
    void setStaticPeers(const QStringList &peers);

signals:
    void ready();
    void runningChanged();
    void statusChanged();

private slots:
    void onStarted();
    void onError(QProcess::ProcessError error);
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onStderr();

private:
    QString executablePath() const;
    void    extractIfNeeded();
    void    setStatus(const QString &s);
    void    setRunning(bool r);

    QProcess   *m_proc    = nullptr;
    bool        m_running = false;
    QString     m_status;
    QString     m_passphrase;
    QStringList m_peers;
};
