#ifndef CLIENT_H
#define CLIENT_H

#include <QWidget>
#include <chatclient.h>
#include <QScreen>
#include <QGuiApplication>
#include <selfinfowidget.h>
#include <QListWidgetItem>
#include <QInputDialog>
#include <QBuffer>
#include <QLabel>
#include <QPixmap>
#include <QJsonParseError>

namespace Ui {
class Client;
}

class Client : public QWidget
{
    Q_OBJECT

public:
    explicit Client(QWidget *parent = nullptr);
    ~Client();


    void RefreshFriendList();
    void RefreshGroupList();


    void setSocket(QTcpSocket *socket);

    void setUserName(QString name); // 【新增】

private slots:
    void on_chat_back_clicked();

    void on_pushBtn_hide_2_clicked();


    void on_pushBtn_close_2_clicked();

    void on_pushBtn_refresh_2_clicked();

    void on_pushButton_emoj_4_clicked();

    void on_pushButton_msg_list_2_clicked();

    void on_pushButton_friend_list_2_clicked();

    void onReadyRead();

    void on_pushBtn_send_2_clicked();

    void on_pushButton_addFriend_2_clicked();

    void on_pushButton_image_2_clicked();

private:
    Ui::Client *ui;

    bool        m_isFull;
    QRect       m_rect;

    int curListWidgetIndex;

    QString m_currentAccount; // 用于记录当前登录的是谁
    QTcpSocket *m_socket;     // 确保你有这个


    QListWidget *m_friendList; // 我们给它起个标准点的名字

    // 如果你有群组列表，也可以一起改了
    QListWidget *m_groupList;

    QString m_currentName;
    bool m_isLoading; // 【新增】是否正在加载中

    QListWidget *m_chatList; // 用来显示聊天记录的列表
    QString m_chatTarget;    // 记录当前正在跟谁聊天
    void addImageToChatList(const QPixmap& pix, bool isMe);
    // 【新增】用于暂存收到的不完整数据
    QByteArray m_recvBuffer;

};

#endif // CLIENT_H
