#include "netcamerapage.h"
#include "ui_netcamerapage.h"
#include "desktopidle.h"

#include <QCoreApplication>
#include <QAbstractSocket>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QScrollBar>
#include <QScroller>
#include <QTextCursor>
#include <QTextOption>

namespace
{
constexpr int kMaxLogLines = 120;

bool isVideoDeviceToken(const QString &token)
{
    return token.startsWith("/dev/video") || token.startsWith("video", Qt::CaseInsensitive);
}

QString stripValuePrefix(const QString &token)
{
    const int eq = token.indexOf('=');
    if (eq < 0) {
        return token;
    }
    return token.mid(eq + 1).trimmed();
}

QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool hasFile(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile();
}

bool hasDir(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isDir();
}

QString firstExistingFile(const QStringList &paths)
{
    for (const QString &path : paths) {
        if (hasFile(path)) {
            return normalizedPath(path);
        }
    }
    return {};
}

}

NetCameraPage::NetCameraPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::NetCameraPage),
    m_streamProcess(new QProcess(this)),
    m_infoPanel(nullptr),
    m_stopping(false)
{
    ui->setupUi(this);

    setWindowFlags(Qt::FramelessWindowHint);
    ui->edtAddress->setPlaceholderText("lan / wan / /dev/video0 / file=/path/video.mp4");
    if (ui->edtAddress->text().trimmed().isEmpty()) {
        ui->edtAddress->setText("/dev/video0");
    }

    m_infoPanel = new QPlainTextEdit(this);
    m_infoPanel->setObjectName("txtInfoPanel");
    m_infoPanel->setGeometry(ui->lblDisplay->geometry());
    m_infoPanel->setReadOnly(true);
    m_infoPanel->setUndoRedoEnabled(false);
    m_infoPanel->setFrameShape(QFrame::NoFrame);
    m_infoPanel->setFocusPolicy(Qt::NoFocus);
    m_infoPanel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_infoPanel->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_infoPanel->setWordWrapMode(QTextOption::WrapAnywhere);
    m_infoPanel->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_infoPanel->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_infoPanel->viewport()->setAttribute(Qt::WA_AcceptTouchEvents, true);
    QScroller::grabGesture(m_infoPanel->viewport(), QScroller::TouchGesture);
    QScroller::grabGesture(m_infoPanel->viewport(), QScroller::LeftMouseButtonGesture);
    m_infoPanel->setStyleSheet(
        "QPlainTextEdit#txtInfoPanel {"
        "background-color:#05070a;"
        "border:2px solid #26324a;"
        "border-radius:8px;"
        "color:#e8f2ff;"
        "font-size:16px;"
        "padding:16px;"
        "selection-background-color:transparent;"
        "}"
        "QScrollBar:vertical {"
        "background:rgba(255,255,255,28);"
        "width:18px;"
        "margin:8px 4px 8px 4px;"
        "border-radius:7px;"
        "}"
        "QScrollBar::handle:vertical {"
        "background:#53d7ff;"
        "min-height:46px;"
        "border-radius:7px;"
        "}"
        "QScrollBar::add-line:vertical,"
        "QScrollBar::sub-line:vertical {"
        "height:0px;"
        "}"
        "QScrollBar::add-page:vertical,"
        "QScrollBar::sub-page:vertical {"
        "background:transparent;"
        "}");
    ui->lblDisplay->hide();
    m_infoPanel->raise();
    ui->lblStatus->setText("Status: idle");

    m_streamProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_streamProcess, &QProcess::started, this, &NetCameraPage::onProcessStarted);
    connect(m_streamProcess, &QProcess::readyRead, this, &NetCameraPage::onProcessReadyRead);
    connect(m_streamProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &NetCameraPage::onProcessFinished);
    connect(m_streamProcess,
            QOverload<QProcess::ProcessError>::of(&QProcess::errorOccurred),
            this,
            &NetCameraPage::onProcessError);

    setStreamingUi(false);
    m_localAddress = detectLocalIPv4();
    appendLogLine("WebRTC 网络摄像头未启动。");
    appendLogLine("默认启用 test.mp4 作为第二路；输入 file=/path/video.mp4 可替换第二路。");
}

NetCameraPage::~NetCameraPage()
{
    stopStreamProcess();
    delete ui;
}

void NetCameraPage::on_btnBack_clicked()
{
    stopStreamProcess();
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}

void NetCameraPage::on_btnConnect_clicked()
{
    startStreamProcess();
}

void NetCameraPage::on_btnDisconnect_clicked()
{
    stopStreamProcess();
    setStreamingUi(false);
}

void NetCameraPage::onProcessStarted()
{
    m_stopping = false;
    setStreamingUi(true);
    appendLogLine("推流进程已启动，等待 ZLMediaKit 发布地址...");
}

void NetCameraPage::onProcessReadyRead()
{
    const QString output = QString::fromUtf8(m_streamProcess->readAll());
    const QStringList lines = output.split('\n');
    for (QString line : lines) {
        line.remove('\r');
        line = line.trimmed();
        if (!line.isEmpty()) {
            appendLogLine(line);
        }
    }
}

void NetCameraPage::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const bool wasStopping = m_stopping;
    m_stopping = false;
    setStreamingUi(false);

    if (wasStopping) {
        appendLogLine("推流服务已停止。");
        return;
    }

    const QString status = exitStatus == QProcess::NormalExit ? "正常退出" : "异常退出";
    appendLogLine(QString("推流服务%1，退出码=%2。").arg(status).arg(exitCode));
}

void NetCameraPage::onProcessError(QProcess::ProcessError)
{
    setStreamingUi(false);
    appendLogLine("推流进程错误: " + m_streamProcess->errorString());
}

void NetCameraPage::startStreamProcess()
{
    if (m_streamProcess->state() != QProcess::NotRunning) {
        appendLogLine("推流服务已经在运行。");
        return;
    }

    m_projectRoot = resolveStreamProjectRoot();
    if (m_projectRoot.isEmpty()) {
        appendLogLine("错误: 找不到 src/edge_media_pipeline 目录。");
        return;
    }

    m_appPath = resolveStreamAppPath(m_projectRoot);
    if (m_appPath.isEmpty()) {
        appendLogLine("错误: 找不到 WebRTC 推流程序，请先在 src/edge_media_pipeline 编译 build/app。");
        return;
    }

    m_modelPath = resolvePersonModelPath(m_projectRoot);
    if (m_modelPath.isEmpty()) {
        appendLogLine("错误: 找不到人体识别模型 models/yolov6n_85.rknn。");
        return;
    }

    m_profile = "lan";
    m_cameraDevice = "/dev/video0";
    m_fileInput.clear();
    bool defaultSecondInputMissing = false;

    QString input = ui->edtAddress->text().trimmed();
    input.replace(';', ' ');
    input.replace(',', ' ');
    const QStringList tokens = input.split(' ', QString::SkipEmptyParts);
    for (const QString &rawToken : tokens) {
        const QString token = rawToken.trimmed();
        if (token.isEmpty()) {
            continue;
        }

        if (token.compare("wan", Qt::CaseInsensitive) == 0) {
            m_profile = "wan";
            continue;
        }
        if (token.compare("lan", Qt::CaseInsensitive) == 0) {
            m_profile = "lan";
            continue;
        }

        if (token.startsWith("camera=", Qt::CaseInsensitive)) {
            const QString value = stripValuePrefix(token);
            if (!value.isEmpty()) {
                m_cameraDevice = value;
            }
            continue;
        }

        if (token.startsWith("file=", Qt::CaseInsensitive) ||
            token.startsWith("input=", Qt::CaseInsensitive) ||
            token.startsWith("rtsp=", Qt::CaseInsensitive)) {
            const QString value = stripValuePrefix(token);
            if (!value.isEmpty()) {
                m_fileInput = value;
            }
            continue;
        }

        if (isVideoDeviceToken(token)) {
            m_cameraDevice = token;
            continue;
        }

        m_fileInput = token;
    }

    if (m_fileInput.isEmpty()) {
        m_fileInput = resolveDefaultSecondInputPath(m_projectRoot);
        if (m_fileInput.isEmpty()) {
            defaultSecondInputMissing = true;
        }
    }

    m_configPath = resolveZlmConfigPath(m_projectRoot, m_profile);
    m_localAddress = detectLocalIPv4();
    m_logLines.clear();

    QStringList args;
    args << "--camera" << m_cameraDevice;
    args << "--model" << m_modelPath;
    args << (m_profile == "wan" ? "--wan-mode" : "--lan-mode");
    if (!m_configPath.isEmpty()) {
        args << "--zlm-config" << m_configPath;
    }
    if (!m_fileInput.isEmpty()) {
        args << "--file" << m_fileInput;
    }
    args << "--no-audio";

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!m_configPath.isEmpty()) {
        env.insert("RKMEDIA_ZLM_CONFIG", m_configPath);
    }
    env.insert("XIAOMAN_YOLO_PERSON_MODEL", m_modelPath);

    QStringList libDirs;
    const QString zlmLib = QDir(m_projectRoot).absoluteFilePath("lib");
    const QString projectLib = QDir(m_projectRoot).absoluteFilePath("../../lib");
    if (hasDir(zlmLib)) {
        libDirs << normalizedPath(zlmLib);
    }
    if (hasDir(projectLib)) {
        libDirs << normalizedPath(projectLib);
    }
    const QString oldLdPath = env.value("LD_LIBRARY_PATH");
    if (!oldLdPath.isEmpty()) {
        libDirs << oldLdPath;
    }
    if (!libDirs.isEmpty()) {
        env.insert("LD_LIBRARY_PATH", libDirs.join(":"));
    }

    m_streamProcess->setProcessEnvironment(env);
    m_streamProcess->setWorkingDirectory(m_projectRoot);

    setStreamingUi(true);
    appendLogLine("正在启动 WebRTC 推流服务...");
    if (defaultSecondInputMissing) {
        appendLogLine("警告: 默认第二路 test.mp4 不存在，将以单路摄像头启动。");
    }
    appendLogLine("程序: " + m_appPath);
    appendLogLine("摄像头: " + m_cameraDevice);
    appendLogLine("第二路: " + (m_fileInput.isEmpty() ? QString("<未启用>") : m_fileInput));
    appendLogLine("模型: " + m_modelPath);
    appendLogLine("配置: " + (m_configPath.isEmpty() ? QString("<fallback>") : m_configPath));
    appendLogLine("本机IP: " + (m_localAddress.isEmpty() ? QString("<未检测到>") : m_localAddress));
    appendLogLine("命令: " + m_appPath + " " + args.join(' '));
    if (!m_localAddress.isEmpty()) {
        appendLogLine("PC播放页: http://" + m_localAddress + ":8000/webrtc_play_test.html");
        appendLogLine("WebRTC API: http://" + m_localAddress +
                      ":8000/index/api/webrtc?app=live&stream=camera&type=play");
    }

    m_streamProcess->start(m_appPath, args);
}

void NetCameraPage::stopStreamProcess()
{
    if (m_streamProcess->state() == QProcess::NotRunning) {
        return;
    }

    m_stopping = true;
    appendLogLine("正在停止推流服务...");
    m_streamProcess->terminate();
    if (!m_streamProcess->waitForFinished(1500)) {
        appendLogLine("推流服务未及时退出，强制结束。");
        m_streamProcess->kill();
        m_streamProcess->waitForFinished(1500);
    }
}

void NetCameraPage::appendLogLine(const QString &line)
{
    QString text = line;
    if (!m_localAddress.isEmpty()) {
        text.replace("<device-ip>", m_localAddress);
    }
    qInfo().noquote() << "[NetCamera]" << text;
    m_logLines.append(text);
    while (m_logLines.size() > kMaxLogLines) {
        m_logLines.removeFirst();
    }
    refreshInfoPanel();
}

void NetCameraPage::refreshInfoPanel()
{
    const bool running = m_streamProcess->state() != QProcess::NotRunning;
    QString text;
    text += "WebRTC 网络摄像头\n";
    text += "状态: " + QString(running ? "运行中" : "未运行") + "\n";
    text += "模式: " + (m_profile.isEmpty() ? QString("lan") : m_profile) + "\n";
    text += "摄像头: " + (m_cameraDevice.isEmpty() ? QString("/dev/video0") : m_cameraDevice) + "\n";
    text += "第二路: " + (m_fileInput.isEmpty() ? QString("<未启用>") : m_fileInput) + "\n";
    text += "本机IP: " + (m_localAddress.isEmpty() ? QString("<未检测到>") : m_localAddress) + "\n";
    text += "\n连接信息 / 运行日志:\n";
    text += m_logLines.join("\n");
    if (m_infoPanel) {
        QScrollBar *bar = m_infoPanel->verticalScrollBar();
        const int oldScrollValue = bar ? bar->value() : 0;
        const bool followTail = (bar == nullptr) || (bar->value() >= bar->maximum() - 8);
        m_infoPanel->setPlainText(text);
        if (followTail) {
            m_infoPanel->moveCursor(QTextCursor::End);
            if (bar) {
                bar->setValue(bar->maximum());
            }
        } else if (bar) {
            bar->setValue(qMin(oldScrollValue, bar->maximum()));
        }
    } else {
        ui->lblDisplay->setText(text);
    }
}

void NetCameraPage::setStreamingUi(bool running)
{
    ui->btnConnect->setEnabled(!running);
    ui->btnDisconnect->setEnabled(running);
    ui->edtAddress->setEnabled(!running);
    ui->lblStatus->setText(running ? "Status: streaming" : "Status: stopped");
}

QString NetCameraPage::detectLocalIPv4() const
{
    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress &address : addresses) {
        const QString text = address.toString();
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !text.startsWith("127.")) {
            return text;
        }
    }
    return {};
}

QString NetCameraPage::resolveStreamProjectRoot() const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    const QStringList candidates{
        QDir(appDir).absoluteFilePath("../src/edge_media_pipeline"),
        QDir(cwd).absoluteFilePath("../src/edge_media_pipeline"),
        QDir(appDir).absoluteFilePath("src/edge_media_pipeline"),
        QDir(cwd).absoluteFilePath("src/edge_media_pipeline"),
        "/home/orangepi/cpp/friday_voice_speaker/src/edge_media_pipeline",
    };

    for (const QString &candidate : candidates) {
        const QString cmakeFile = QDir(candidate).absoluteFilePath("CMakeLists.txt");
        if (hasDir(candidate) && hasFile(cmakeFile)) {
            return normalizedPath(candidate);
        }
    }
    return {};
}

QString NetCameraPage::resolveStreamAppPath(const QString &projectRoot) const
{
    return firstExistingFile({
        QDir(projectRoot).absoluteFilePath("build/app"),
        QDir(projectRoot).absoluteFilePath("build/bin/app"),
        QDir(projectRoot).absoluteFilePath("build_xiaoman/app"),
        QDir(projectRoot).absoluteFilePath("build-xiaoman/app"),
        QDir(projectRoot).absoluteFilePath("app"),
    });
}

QString NetCameraPage::resolvePersonModelPath(const QString &projectRoot) const
{
    return firstExistingFile({
        QDir(projectRoot).absoluteFilePath("../../models/yolov6n_85.rknn"),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../models/yolov6n_85.rknn"),
        QDir::current().absoluteFilePath("../models/yolov6n_85.rknn"),
        QDir::current().absoluteFilePath("models/yolov6n_85.rknn"),
        "/home/orangepi/cpp/friday_voice_speaker/models/yolov6n_85.rknn",
    });
}

QString NetCameraPage::resolveDefaultSecondInputPath(const QString &projectRoot) const
{
    return firstExistingFile({
        QDir(projectRoot).absoluteFilePath("test.mp4"),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../src/edge_media_pipeline/test.mp4"),
        QDir::current().absoluteFilePath("../src/edge_media_pipeline/test.mp4"),
        QDir::current().absoluteFilePath("test.mp4"),
        "/home/orangepi/cpp/friday_voice_speaker/src/edge_media_pipeline/test.mp4",
    });
}

QString NetCameraPage::resolveZlmConfigPath(const QString &projectRoot, const QString &profile) const
{
    const QString fileName = profile == "wan" ? "config.wan.ini" : "config.lan.ini";
    return firstExistingFile({
        QDir(projectRoot).absoluteFilePath(fileName),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../src/edge_media_pipeline/" + fileName),
        QDir::current().absoluteFilePath("../src/edge_media_pipeline/" + fileName),
    });
}
