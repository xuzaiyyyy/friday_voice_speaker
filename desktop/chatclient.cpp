#include "chatclient.h"
#include "ui_chatclient.h"
#include "desktopidle.h"

ChatClient::ChatClient(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ChatClient)
{
    ui->setupUi(this);

    this->setWindowFlags(Qt::FramelessWindowHint);
    ui->stackedWidget->setCurrentIndex(0);

    initNetwork();
}

ChatClient::~ChatClient()
{
    delete ui;
}

void ChatClient::initNetwork() {
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &ChatClient::onConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &ChatClient::onReadyRead);

    // 连接到你的服务器 IP 和 端口 (9006)
    m_socket->connectToHost("********", 8080);
}


void ChatClient::on_pushBtn_hide_clicked()
{
    QWidget* pWindow = this->window();
    if(pWindow->isWindow())
    {
        pWindow->raise();
        pWindow->activateWindow();
    }
}

void ChatClient::on_pushBtn_close_clicked()
{
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}


void ChatClient::on_pushBtn_hide_2_clicked()
{
    QWidget* pWindow = this->window();
    if(pWindow->isWindow())
    {
        pWindow->raise();
        pWindow->activateWindow();
    }
}

void ChatClient::on_pushBtn_close_2_clicked()
{
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}


void ChatClient::PageSwitch(int pageIndex)
{
    int curIndex = ui->stackedWidget->currentIndex();
    {
        if (pageIndex < 0 || pageIndex >= ui->stackedWidget->count()) {
            return;
        }
        QWidget *currentPage = ui->stackedWidget->currentWidget();
        QWidget *targetPage = ui->stackedWidget->widget(pageIndex);
        targetPage->setGeometry(0,0,currentPage->width(),currentPage->height());

        if (!currentPage || !targetPage || currentPage == targetPage) {
            return;
        }

        int currentPageX = currentPage->x();
        int targetPageX = targetPage->x();
        int currentPageWidth = currentPage->width();
        // 创建当前页面向左滑出的动画
        QPropertyAnimation *currentPageAnimation = new QPropertyAnimation(currentPage, "geometry");
        currentPageAnimation->setDuration(400);
        currentPageAnimation->setEasingCurve(QEasingCurve::InOutQuad);
        // 创建目标页面从右至左滑进的动画
        QPropertyAnimation *targetPageAnimation = new QPropertyAnimation(targetPage, "geometry");
        targetPageAnimation->setDuration(400);
        targetPageAnimation->setEasingCurve(QEasingCurve::InOutQuad);
if(pageIndex == 1)
{
    currentPageAnimation->setStartValue(QRect(currentPageX, currentPage->y(), currentPageWidth, currentPage->height()));
    currentPageAnimation->setEndValue(QRect(currentPageX - currentPageWidth, currentPage->y(), currentPageWidth, currentPage->height()));

    targetPageAnimation->setStartValue(QRect(targetPageX + currentPageWidth, targetPage->y(), targetPage->width(), targetPage->height()));
    targetPageAnimation->setEndValue(QRect(targetPageX, targetPage->y(), targetPage->width(), targetPage->height()));
}
else
{
    currentPageAnimation->setStartValue(QRect(currentPageX, currentPage->y(), currentPageWidth, currentPage->height()));
    currentPageAnimation->setEndValue(QRect(currentPageX + currentPageWidth, currentPage->y(), currentPageWidth, currentPage->height()));

    targetPageAnimation->setStartValue(QRect(targetPageX - currentPageWidth, targetPage->y(), targetPage->width(), targetPage->height()));
    targetPageAnimation->setEndValue(QRect(targetPageX, targetPage->y(), targetPage->width(), targetPage->height()));
}
        // 创建动画组，添加动画，并启动动画组
        QParallelAnimationGroup *animationGroup = new QParallelAnimationGroup();
        animationGroup->addAnimation(currentPageAnimation);
        animationGroup->addAnimation(targetPageAnimation);
        ui->stackedWidget->widget(pageIndex)->setVisible(true);
        ui->stackedWidget->widget(pageIndex)->update();
        ui->stackedWidget->widget(pageIndex)->show();
        animationGroup->start(QAbstractAnimation::DeleteWhenStopped);

        animationGroup->setProperty(
                "widget", QVariant::fromValue(ui->stackedWidget->widget(curIndex)));

        connect(animationGroup, &QParallelAnimationGroup::finished, [=]() {
            // 切换页面
            ui->stackedWidget->widget(pageIndex)->hide();
            ui->stackedWidget->setCurrentIndex(pageIndex);
                });
    }
}

void ChatClient::on_pushbtn_regist_clicked()
{
    PageSwitch(1);
}

void ChatClient::on_pushButton_return_clicked()
{
    PageSwitch(0);
}


void ChatClient::on_pushButton_seePassword_clicked()
{
    if(ui->lineEdit_password->echoMode() == QLineEdit::Password)
    {
          ui->lineEdit_password->setEchoMode(QLineEdit::Normal);
    }
    else
        ui->lineEdit_password->setEchoMode(QLineEdit::Password);
}

void ChatClient::on_pushButton_login_clicked()
{
    // 1. 获取输入
    QString account = ui->lineEdit_account->text();
    QString password = ui->lineEdit_password->text();

    if(account.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入账号和密码");
        return;
    }

    // 2. 检查连接状态
    if(m_socket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::critical(this, "错误", "未连接到服务器，请检查网络！");
        return;
    }

    // 3. 封装 JSON 协议
    QJsonObject json;
    json["type"] = "login";
    json["username"] = account;
    json["password"] = password;

    // 4. 发送给 C++ 服务器
    QJsonDocument doc(json);
    m_socket->write(doc.toJson());
    qDebug() << "发送登录请求: " << account;
}

void ChatClient::onReadyRead()
{
    QByteArray data = m_socket->readAll();
    qDebug() << "收到服务器消息: " << data;

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if(doc.isNull() || !doc.isObject()) return;

    QJsonObject root = doc.object();
    QString type = root["type"].toString();

    if(type == "register_ack") {
        QString result = root["result"].toString();
        if(result == "ok") {
            QMessageBox::information(this, "成功", "注册成功，请返回登录！");
            PageSwitch(0); // 注册成功自动切回登录页面
        } else if(result == "exists") {
            QMessageBox::warning(this, "失败", "该账号已存在！");
        } else {
            QMessageBox::critical(this, "错误", "注册失败，服务器异常。");
        }
    }

    if(type == "login_ack") {
        QString result = root["result"].toString();
        if(result == "ok") {
            // 核心修复：先跳转，后弹窗
            // 1. 检查你的 stackedWidget 布局
            // Page 0: 登录, Page 1: 注册, Page 2: 聊天
            qDebug() << "准备跳转到聊天页面...";

            // 1. 实例化主界面 (如果你之前没实例化)
            // 假设你有一个成员变量 Client *m_mainClient;
            // 或者在这里临时 new 一个，但要注意内存管理
            Client *clientWindow = new Client();

            // 2. 【核心】把当前连接好的 Socket 传给主界面
            // 注意：这里传过去之后，ChatClient 不要 delete 这个 socket
            clientWindow->setSocket(this->m_socket);

            // 【新增这一行】把输入框里的账号传给主界面
            QString account = ui->lineEdit_account->text();
            clientWindow->setUserName(ui->lineEdit_account->text());

            // 3. 这里的信号连接可以断开了，防止 ChatClient 和 Client 同时抢消息
            // (可选，不断开也行，因为 type 不一样)
            disconnect(m_socket, &QTcpSocket::readyRead, this, &ChatClient::onReadyRead);

            replaceDesktopTopLevel(this, clientWindow);

            // 2. 弹窗放在后面，或者不弹窗直接进
            // QMessageBox::information(this, "登录成功", "欢迎回来！");
        } else {
            QMessageBox::critical(this, "登录失败", "账号或密码错误！");
        }
    }
}


void ChatClient::onConnected()
{
    qDebug() << "连接服务器成功！";
}


void ChatClient::on_pushButton_regist_clicked()
{
    QString account = ui->lineEdit_account_2->text();
    QString password = ui->lineEdit_password_2->text();
    QString confirm = ui->lineEdit_password_3->text();

    // 1. 基础校验：只检查账号和密码
    if(account.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入账号和密码！");
        return;
    }
    if(password != confirm) {
        QMessageBox::warning(this, "提示", "两次输入的密码不一致！");
        return;
    }

    // 2. 构造 JSON：不包含 nickname 字段
    QJsonObject json;
    json["type"] = "register";
    json["username"] = account;
    json["password"] = password;

    m_socket->write(QJsonDocument(json).toJson());
}
