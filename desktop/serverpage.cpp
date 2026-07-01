#include "serverpage.h"
#include "desktopidle.h"

ServerPage::ServerPage(QWidget *parent) : QWidget(parent), m_isRunning(false)
{
    setupUI();
}

void ServerPage::setupUI()
{
    this->resize(1024, 600);
    this->setWindowFlags(Qt::FramelessWindowHint);

    // 整体背景：深色极客风
    this->setStyleSheet("background-color: #1E1E1E; color: #00FF00; font-family: 'Consolas', 'Monospace';");

    // === 顶部控制栏 ===
    QHBoxLayout *topLayout = new QHBoxLayout();

    m_btnBack = new QPushButton("Back");
    m_btnBack->setFixedSize(80, 30);
    m_btnBack->setStyleSheet("background-color: #555; color: white; border-radius: 5px;");

    // 连接到内部槽函数 onBackBtnClicked
    connect(m_btnBack, &QPushButton::clicked, this, &ServerPage::onBackBtnClicked);

    QLabel *lblPort = new QLabel("Port:");
    lblPort->setStyleSheet("color: white; font-weight: bold; font-size: 14px;");

    m_portInput = new QLineEdit("9006");
    m_portInput->setFixedWidth(80);
    m_portInput->setStyleSheet("background-color: #333; color: white; border: 1px solid #555; padding: 2px; font-size: 14px;");

    m_statusLabel = new QLabel("STOPPED");
    m_statusLabel->setStyleSheet("color: #FF3B30; font-weight: bold; border: 2px solid #FF3B30; padding: 2px 8px; border-radius: 4px;");

    topLayout->addWidget(m_btnBack);
    topLayout->addStretch();
    topLayout->addWidget(lblPort);
    topLayout->addWidget(m_portInput);
    topLayout->addSpacing(20);
    topLayout->addWidget(m_statusLabel);

    // === 中间：日志控制台 (黑底绿字) ===
    m_console = new QTextEdit();
    m_console->setReadOnly(true);
    // 稍微调大了一点字体，适应 1024x600 屏幕
    m_console->setStyleSheet("background-color: black; color: #00FF00; border: 1px solid #444; font-size: 14px; line-height: 150%;");
    m_console->setPlaceholderText("System logs will appear here...");

    // === 底部：操作按钮 ===
    QHBoxLayout *bottomLayout = new QHBoxLayout();

    m_btnStart = new QPushButton("START SERVER");
    m_btnStart->setFixedHeight(50); // 稍微加高按钮，方便点击
    m_btnStart->setStyleSheet("QPushButton { background-color: #34C759; color: white; font-weight: bold; border-radius: 8px; font-size: 16px; }"
                              "QPushButton:hover { background-color: #30B753; }"
                              "QPushButton:disabled { background-color: #555; color: #888; }");

    m_btnStop = new QPushButton("SHUTDOWN");
    m_btnStop->setFixedHeight(50);
    m_btnStop->setStyleSheet("QPushButton { background-color: #FF3B30; color: white; font-weight: bold; border-radius: 8px; font-size: 16px; }"
                             "QPushButton:hover { background-color: #E0352A; }"
                             "QPushButton:disabled { background-color: #555; color: #888; }");
    m_btnStop->setEnabled(false);

    connect(m_btnStart, &QPushButton::clicked, this, &ServerPage::onStartClicked);
    connect(m_btnStop, &QPushButton::clicked, this, &ServerPage::onStopClicked);

    bottomLayout->addWidget(m_btnStart);
    bottomLayout->addSpacing(20);
    bottomLayout->addWidget(m_btnStop);

    // === 总布局 ===
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    // 增加一点边距，让界面不那么贴边
    mainLayout->setContentsMargins(18, 18, 18, 18);
    mainLayout->setSpacing(12);
    mainLayout->addLayout(topLayout);
    mainLayout->addWidget(m_console);
    mainLayout->addLayout(bottomLayout);
}

void ServerPage::log(const QString &msg)
{
    QString timestamp = QDateTime::currentDateTime().toString("[hh:mm:ss] ");
    m_console->append(timestamp + msg);
}

void ServerPage::onStartClicked()
{
    int port = m_portInput->text().toInt();

    // 1. 创建并启动线程
    m_thread = new ServerThread(port, this);

    // 2. 连接日志信号 (让线程里的 logMessage 显示到你的黑框里)
    connect(m_thread, &ServerThread::logMessage, this, &ServerPage::log);

    // 3. 线程结束时自动回收内存
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);

    m_thread->start(); // 🚀 启动子线程

    m_isRunning = true;
    m_portInput->setEnabled(false);
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);

    m_statusLabel->setText("RUNNING");
    m_statusLabel->setStyleSheet("color: #34C759; font-weight: bold; border: 2px solid #34C759; padding: 2px 8px; border-radius: 4px;");

    log("Initializing TinyWebServer...");
    log("Epoll reactor created.");
    log("Thread pool initialized with 8 threads.");
    log(QString("Server listening on port %1").arg(m_portInput->text()));
    log("Ready to accept connections.");
}

void ServerPage::onStopClicked()
{
    if (m_thread) {
        log("Stopping server thread...");

        // 优雅停止
        m_thread->stopServer();

        // 因为 WebServer 用了 epoll_wait，可能要等几秒超时才能退出
        // 这里只是发出了停止信号

        m_thread = nullptr;
    }

    m_isRunning = false;
    m_portInput->setEnabled(true);
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);

    m_statusLabel->setText("STOPPED");
    m_statusLabel->setStyleSheet("color: #FF3B30; font-weight: bold; border: 2px solid #FF3B30; padding: 2px 8px; border-radius: 4px;");

    log("Server shutdown sequence initiated.");
    log("All resources released.");
}

// 【关键修改】不要在这里 new MainWindow，而是发送信号
void ServerPage::onBackBtnClicked()
{
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}
