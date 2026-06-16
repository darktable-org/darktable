#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

// Manages the lifecycle of the dt-p2p-daemon subprocess.
class DaemonManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool        running     READ isRunning    NOTIFY runningChanged)
    Q_PROPERTY(QString     status      READ status       NOTIFY statusChanged)
    Q_PROPERTY(QString     passphrase  READ passphrase   NOTIFY passphraseChanged)
    Q_PROPERTY(QStringList staticPeers READ staticPeers  NOTIFY staticPeersChanged)
    Q_PROPERTY(QStringList logLines    READ logLines     NOTIFY logLinesChanged)

public:
    explicit DaemonManager(QObject *parent = nullptr);
    ~DaemonManager();

    bool        isRunning()   const { return m_running;      }
    QString     status()      const { return m_status;       }
    QStringList staticPeers() const { return m_staticPeers;  }
    QStringList logLines()    const { return m_logLines;     }

    QString socketPath() const;
    QString proxyDir()   const;
    QString importDir()  const;
    QString passphrase() const;

public slots:
    void start();
    void stop();
    void restart();
    void setPassphrase(const QString &passphrase);
    void setStaticPeers(const QStringList &peers);
    void clearLog();

signals:
    void ready();
    void runningChanged();
    void statusChanged();
    void passphraseChanged();
    void staticPeersChanged();
    void logLinesChanged();
    void deepLinkReceived(const QString &url);

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
    void    appendLog(const QString &line);

    static constexpr int kMaxLogLines = 300;

    QProcess   *m_proc       = nullptr;
    bool        m_running    = false;
    QString     m_status;
    QString     m_passphrase;
    QStringList m_staticPeers;
    QStringList m_logLines;
};
