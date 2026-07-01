#ifndef SERVERTHREAD_H
#define SERVERTHREAD_H

#include <QThread>
#include <QDebug>
#include "webserver.h" // 包含你的核心头文件

class ServerThread : public QThread
{
    Q_OBJECT
public:
    // 构造函数：接收端口号
    explicit ServerThread(int port, QObject *parent = nullptr);
    ~ServerThread();

    // 停止服务器的接口
    void stopServer();

signals:
    // 用于把 Log 发送给 UI 显示
    void logMessage(const QString &msg);

protected:
    // 线程入口函数
    void run() override;

private:
    int m_port;
    WebServer *m_server; // 持有 WebServer 指针
};

#endif // SERVERTHREAD_H
