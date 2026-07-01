#ifndef CHATCLIENT_H
#define CHATCLIENT_H

#include <QWidget>
#include <mainwindow.h>
#include <QParallelAnimationGroup>
#include <client.h>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMessageBox>

namespace Ui {
class ChatClient;
}

class ChatClient : public QWidget
{
    Q_OBJECT

public:
    explicit ChatClient(QWidget *parent = nullptr);
    ~ChatClient();

private slots:
    void on_pushBtn_hide_clicked();

    void on_pushBtn_close_clicked();

    void PageSwitch(int pageIndex);

    void on_pushbtn_regist_clicked();

    void on_pushButton_return_clicked();

    void on_pushBtn_hide_2_clicked();

    void on_pushBtn_close_2_clicked();

    void on_pushButton_seePassword_clicked();

    void on_pushButton_login_clicked();

    void onReadyRead();      // 处理服务器回传的数据
    void onConnected();      // 连接成功处理

    void on_pushButton_regist_clicked();

private:
    Ui::ChatClient *ui;

    QTcpSocket *m_socket;    // 通信套接字
    void initNetwork();      // 初始化网络连接


};

#endif // CHATCLIENT_H
