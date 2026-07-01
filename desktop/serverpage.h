#ifndef SERVERPAGE_H
#define SERVERPAGE_H

#include <QWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <mainwindow.h>
#include <serverthread.h>

class ServerPage : public QWidget
{
    Q_OBJECT
public:
    explicit ServerPage(QWidget *parent = nullptr);

signals:
    // 信号：告诉主窗口“我要返回”
    void backBtnClicked();

private slots:
    void onStartClicked();
    void onStopClicked();
    void onBackBtnClicked(); // 内部槽函数：处理按钮点击

private:
    void setupUI();
    void log(const QString &msg);
    ServerThread *m_thread;

    QTextEdit *m_console;
    QLineEdit *m_portInput;
    QPushButton *m_btnStart;
    QPushButton *m_btnStop;
    QPushButton *m_btnBack;
    QLabel *m_statusLabel;

    bool m_isRunning;
};

#endif // SERVERPAGE_H
