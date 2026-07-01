#include "serverthread.h"

ServerThread::ServerThread(int port, QObject *parent)
    : QThread(parent), m_port(port), m_server(nullptr)
{
}

ServerThread::~ServerThread()
{
    // 线程销毁时，告诉 Log 系统不要再发消息了
    Log::Instance()->SetCallback(nullptr);

    stopServer();
    wait(); // 等待线程彻底退出，防止崩溃

    if (m_server) {
        delete m_server;
        m_server = nullptr;
    }
}

void ServerThread::stopServer()
{
    if (m_server) {
        // 调用我们在第二步加的刹车函数
        m_server->Stop();
    }
}

void ServerThread::run()
{
    // 1. 设置回调：当 Log 系统有消息时，通过 emit logMessage 发送出去
    Log::Instance()->SetCallback([this](const std::string& msg){
        // 去掉末尾的换行符，因为 QTextEdit 会自动换行
        QString qMsg = QString::fromStdString(msg).trimmed();
        emit logMessage(qMsg);
    });

    emit logMessage("Thread: Initializing TinyWebServer...");

    try {
        // === 这里填入 WebServer 的构造参数 ===
        // 这里的参数对应你 webserver.cpp 里的构造函数
        // ⚠️ 请务必修改数据库密码为你的实际密码
        m_server = new WebServer(
            m_port, 3, 60000, false,             // 端口, ET模式, 超时60s, 优雅关闭
            3306, "root", "********", "webserver",    // 🚨 MySQL配置: 端口, 用户, 密码, 库名
            12, 6, true, 1, 1024                 // 连接池数量, 线程池数量, 开启日志, 日志等级, 队列容量
        );

        emit logMessage(QString("Thread: Server started on port %1").arg(m_port));

        // 启动服务器循环 (这行代码会一直阻塞，直到调用 Stop)
        m_server->Start();

        emit logMessage("Thread: Server loop exited safely.");

    } catch (std::exception &e) {
        emit logMessage(QString("Error: %1").arg(e.what()));
    } catch (...) {
        emit logMessage("Error: Unknown exception occurred.");
    }

    // 清理资源
    if (m_server) {
        delete m_server;
        m_server = nullptr;
    }
}
