#ifndef NETCAMERAPAGE_H
#define NETCAMERAPAGE_H

#include <QWidget>
#include <mainwindow.h>
#include <QProcess>
#include <QStringList>

class QPlainTextEdit;

namespace Ui {
class NetCameraPage;
}

class NetCameraPage : public QWidget
{
    Q_OBJECT

public:
    explicit NetCameraPage(QWidget *parent = nullptr);
    ~NetCameraPage();

private slots:
    void on_btnBack_clicked();
    void on_btnConnect_clicked();
    void on_btnDisconnect_clicked();
    void onProcessStarted();
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

private:
    Ui::NetCameraPage *ui;

    QProcess *m_streamProcess;
    QPlainTextEdit *m_infoPanel;
    QStringList m_logLines;
    QString m_projectRoot;
    QString m_appPath;
    QString m_modelPath;
    QString m_configPath;
    QString m_cameraDevice;
    QString m_fileInput;
    QString m_profile;
    QString m_localAddress;
    bool m_stopping;

    void startStreamProcess();
    void stopStreamProcess();
    void appendLogLine(const QString &line);
    void refreshInfoPanel();
    void setStreamingUi(bool running);
    QString detectLocalIPv4() const;
    QString resolveStreamProjectRoot() const;
    QString resolveStreamAppPath(const QString &projectRoot) const;
    QString resolvePersonModelPath(const QString &projectRoot) const;
    QString resolveDefaultSecondInputPath(const QString &projectRoot) const;
    QString resolveZlmConfigPath(const QString &projectRoot, const QString &profile) const;
};

#endif // NETCAMERAPAGE_H
