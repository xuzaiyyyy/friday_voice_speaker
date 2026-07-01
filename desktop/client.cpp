#include "client.h"
#include "ui_client.h"
#include "desktopidle.h"

Client::Client(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Client)
{
    ui->setupUi(this);

    m_isLoading = false; // 【新增】默认为 false

    this->setWindowFlags(Qt::FramelessWindowHint);

    // 【新增：初始化好友列表 UI】
    m_friendList = new QListWidget(this);

    // 设置一点样式（可选：去边框，让它融为一体）
    m_friendList->setStyleSheet("QListWidget { border: none; background-color: white; color: black; font-size: 14px; } QListWidget::item { padding: 10px; }");

    // 把列表添加到 stackedWidget_list 的某一页（比如第 1 页）
    // 注意：假设 page_7 是好友列表页，或者我们直接 addWidget
    ui->stackedWidget_list_2->addWidget(m_friendList);

    // 1. 【关键】设置图标大小 (即使暂时没图标，这也能撑开文字的垂直对齐)
    m_friendList->setIconSize(QSize(40, 40));

    // 2. 【关键】开启自动滚动条 (默认就是开的，这里强制确认一下)
    m_friendList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); // 内容多了自动出滚动条
    m_friendList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); //以此保证只上下滚

    // 3. 【精修样式表】让列表项变得宽敞、清晰
    m_friendList->setStyleSheet(
        "QListWidget {"
        "   background-color: rgb(250, 250, 250);"  /* 整体背景极淡的灰，护眼 */
        "   border: none;"                          /* 去边框 */
        "   outline: none;"                         /* 去虚线框 */
        "}"
        "QListWidget::item {"
        "   min-height: 70px;"       /* 【重点】每一行强制给 70px 高度！绝不会遮挡 */
        "   border-bottom: 1px solid rgb(230, 230, 230);" /* 淡淡的分割线 */
        "   padding-left: 10px;"     /* 文字离左边远一点 */
        "}"
        "QListWidget::item:hover {"
        "   background-color: rgb(240, 240, 240);"  /* 鼠标放上去变深一点 */
        "}"
        "QListWidget::item:selected {"
        "   background-color: rgb(220, 220, 220);"  /* 选中时的颜色 */
        "   color: black;"                          /* 选中文字保持黑色 */
        "   border-left: 4px solid #1AAD19;"        /* 左边加个绿条，提示选中 */
        "}"
        /* 美化滚动条 (可选，让滚动条不那么丑) */
        "QScrollBar:vertical {"
        "   width: 8px;"
        "   background: transparent;"
        "}"
        "QScrollBar::handle:vertical {"
        "   background: rgb(200, 200, 200);"
        "   border-radius: 4px;"
        "}"
    );


    // 【1. 初始化聊天记录列表】
    // 我们把它加到 stackedWidget_2 的第一页 (index 0)
    // 如果你的 page_3 是空白页，page_4 是聊天页，请根据实际情况调整 widget(0) 或 widget(1)
    QWidget *chatPage = ui->stackedWidget_2->widget(0);

    // 给这个页面加个布局，让列表自动撑满
    QVBoxLayout *layout = new QVBoxLayout(chatPage);
    layout->setContentsMargins(0,0,0,0); // 去掉边距

    m_chatList = new QListWidget(chatPage);
    layout->addWidget(m_chatList);

    // 设置样式：去边框，透明背景
    m_chatList->setStyleSheet("border:none; background-color: transparent;");
    m_chatList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // 【2. 双击好友 -> 进入聊天模式】
    connect(m_friendList, &QListWidget::itemDoubleClicked, this, [=](QListWidgetItem *item){
        QString name = item->text();
        m_chatTarget = name; // 记录目标

        // 切换到聊天页面 (显示 m_chatList)
        ui->stackedWidget_2->setCurrentIndex(0);

        // 清空上一场聊天记录 (或者以后做加载历史记录)
        m_chatList->clear();

        qDebug() << "开始与" << name << "聊天";
    });

}

Client::~Client()
{
    delete ui;
}


void Client::setUserName(QString name)
{
    this->m_currentName = name;
}

void Client::on_chat_back_clicked()
{
    auto *c = new ChatClient();
    replaceDesktopTopLevel(this, c);
}

void Client::on_pushBtn_hide_2_clicked()
{
    QWidget* pWindow = this->window();
    if(pWindow->isWindow())
    {
        pWindow->raise();
        pWindow->activateWindow();
    }
}


void Client::on_pushBtn_close_2_clicked()
{
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}

void Client::on_pushBtn_refresh_2_clicked()
{
    switch(curListWidgetIndex) {
        case 0:break;
    case 1:RefreshFriendList();break;
    case 2:RefreshGroupList();break;
        default:
            break;
    }
}

// 刷新好友列表
void Client::RefreshFriendList()
{
    // 1. 如果正在加载，直接退出，防止重复点击
        if (m_isLoading) {
            qDebug() << "正在加载中，请稍候...";
            return;
        }

        if (m_currentName.isEmpty()) return;

        // 2. 上锁
        m_isLoading = true;

        // 3. 【视觉优化】先清空列表，显示一个“加载中...”
        m_friendList->clear();
        QListWidgetItem *loadingItem = new QListWidgetItem(m_friendList);
        loadingItem->setText("正在加载好友...");
        loadingItem->setTextAlignment(Qt::AlignCenter); // 居中显示
        m_friendList->addItem(loadingItem);

        // 4. 发送请求
        QJsonObject json;
        json["type"] = "friend_list";
        json["username"] = m_currentName;

        if(m_socket->state() == QAbstractSocket::ConnectedState) {
            m_socket->write(QJsonDocument(json).toJson());
        }
}

// 刷新群组列表 (同理)
void Client::RefreshGroupList()
{
    if(m_currentAccount.isEmpty()) return;

    QJsonObject json;
    json["type"] = "group_list";
    json["username"] = m_currentAccount;

    if(m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(QJsonDocument(json).toJson());
    }
}

void Client::on_pushButton_emoj_4_clicked()
{
    SelfInfoWidget* w = new SelfInfoWidget;
    w->show();
}

void Client::on_pushButton_msg_list_2_clicked()
{
//    if(curListWidgetIndex == 0)
//    {
//        ui->pushButton_msg_list_2->setChecked(true);
//        return;
//    }
//    else
//    {
//        ui->pushButton_friend_list_2->setChecked(false);
//        ui->pushButton_group_list_2->setChecked(false);
//    }
//    curListWidgetIndex = 0;
//    ui->stackedWidget_list_2->setCurrentWidget(messagesListWidget);
}

// 1. 实现 setSocket 函数
void Client::setSocket(QTcpSocket *socket)
{
    this->m_socket = socket;

    // 【关键的一步】
    // 拿到 Socket 后，立刻绑定它的信号到本类的槽函数！
    // 这样，以后服务器发来的消息（好友列表、聊天内容）就会触发 Client::onReadyRead
    connect(m_socket, &QTcpSocket::readyRead, this, &Client::onReadyRead);

    qDebug() << "Client 主界面已接管网络连接";
}

// 2. 这里就是你刚才写的那个处理好友列表的逻辑
void Client::onReadyRead()
{
    // 1. 【核心修改】把新收到的数据追加到缓冲区屁股后面
    //    而不是直接覆盖
    QByteArray data = m_socket->readAll();
    m_recvBuffer.append(data);

    // 2. 尝试解析缓冲区里的数据
    //    如果是大图片，可能要攒好几次 onReadyRead 才能攒够一个完整的 JSON
    while (!m_recvBuffer.isEmpty()) {

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(m_recvBuffer, &error);

        // ============================================================
        // 情况 A: 解析失败 (数据不完整，或者格式错误)
        // ============================================================
        if (error.error != QJsonParseError::NoError) {
            // 如果是因为数据还没收完 (PrematureEndOfDocument)，我们就 return，
            // 保持 m_recvBuffer 里的数据不动，等待下一次 onReadyRead 带来剩下的数据。
            if (error.error == QJsonParseError::UnterminatedObject) {
                return;
            }

            // 另一种简单判断：如果 JSON 还没以 '}' 结尾，肯定是不完整的，继续等
            if (!m_recvBuffer.trimmed().endsWith("}")) {
                return;
            }

            // 如果确实是格式错误的垃圾数据，为了防止死循环，只能清空
            // (但在正常的网络环境中，通常是因为没收完，上面两个 if 会拦截住)
            qDebug() << "JSON 格式错误，清空缓冲区:" << error.errorString();
            m_recvBuffer.clear();
            return;
        }

        // ============================================================
        // 情况 B: 解析成功！(说明 m_recvBuffer 里已经凑齐了一个完整的 JSON)
        // ============================================================
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            QString type = root["type"].toString();

            // --- 1. 处理好友列表 ---
            if (type == "friend_list_ack") {
                QString result = root["result"].toString();
                if (result == "ok") {
                    m_isLoading = false; // 解锁
                    m_friendList->clear();

                    QJsonArray friends = root["friends"].toArray();
                    for (const QJsonValue &val : friends) {
                        QString friendName = val.toString();
                        QListWidgetItem *item = new QListWidgetItem(m_friendList);
                        item->setText(friendName);

                        // 【强制设置行高，防止挤在一起】
                        item->setSizeHint(QSize(0, 70));
                        item->setIcon(this->style()->standardIcon(QStyle::SP_ComputerIcon)); // 默认头像

                        m_friendList->addItem(item);
                    }
                }
            }
            // --- 2. 处理聊天消息 (文字) ---
            else if (type == "chat") {
                QString sender = root["from"].toString();
                QString msg = root["msg"].toString();

                // 只有当前正在跟这个人聊天，或者为了测试方便直接显示
                // 这里我保留了你的逻辑：sender == m_chatTarget
                if (sender == m_chatTarget) {
                    QListWidgetItem *item = new QListWidgetItem(m_chatList);
                    item->setText(msg);
                    item->setTextAlignment(Qt::AlignLeft);
                    m_chatList->addItem(item);
                    m_chatList->scrollToBottom();
                }
                else {
                    qDebug() << "收到来自" << sender << "的消息，但没在看他";
                }
            }
            // --- 3. 处理添加好友结果 ---
            else if (type == "add_friend_ack") {
                QString result = root["result"].toString();
                if (result == "ok") {
                    QString friendName = root["friend"].toString();
                    QMessageBox::information(this, "成功", "成功添加好友：" + friendName);
                    RefreshFriendList();
                }
                else if (result == "user_not_found") {
                    QMessageBox::warning(this, "失败", "用户不存在。");
                }
                else if (result == "error_self") {
                    QMessageBox::warning(this, "失败", "不能添加自己。");
                }
                else {
                    QMessageBox::warning(this, "失败", "添加失败。");
                }
            }
            // --- 4. 处理图片消息 (Base64) ---
            else if (type == "img") {
                QString sender = root["from"].toString();
                QString base64Str = root["data"].toString();

                if (sender == m_chatTarget) {
                    QByteArray bytes = QByteArray::fromBase64(base64Str.toUtf8());
                    QPixmap pix;
                    pix.loadFromData(bytes);

                    if (!pix.isNull()) {
                        addImageToChatList(pix, false); // false 代表别人发的
                    }
                } else {
                     qDebug() << "收到来自" << sender << "的图片，但没在看他";
                }
            }

            // --- 5. 处理登录结果 (防止你漏掉) ---
            else if (type == "login_ack") {
                 // 如果你在 client.cpp 里处理登录回执，可以在这里加逻辑
                 // 如果你的登录逻辑还在 ChatClient 里，这里可以忽略
            }

            // ============================================================
            // 【关键一步】处理完这个 JSON 后，清空缓冲区！
            // 准备接收下一条全新的消息
            // ============================================================
            m_recvBuffer.clear();

            // 退出循环，等待下一次 readyRead
            break;
        }
        else {
            // 解析出来不是 Object，可能是脏数据
            m_recvBuffer.clear();
            break;
        }
    }
}

void Client::on_pushButton_friend_list_2_clicked()
{
    // 1. 发送网络请求：向服务器索要最新的好友列表
    // (调用我们之前写好的那个函数)
    RefreshFriendList();

    // 2. 切换界面：把中间的堆叠窗口 (StackWidget) 翻到“好友列表”那一页
    // 因为我们在构造函数里是这样写的：ui->stackedWidget_list->addWidget(m_friendList);
    // 所以我们可以直接告诉它“显示 m_friendList 这个控件”
    ui->stackedWidget_list_2->setCurrentWidget(m_friendList);

    // 【备选方案】如果你是在 UI 设计器里直接拖的 Page，可以用索引：
    // ui->stackedWidget_list->setCurrentIndex(1); // 假设好友页是第 2 页 (index 从 0 开始)

    // 3. (可选) 按钮样式反馈
    // 这种导航栏按钮通常是“互斥”的，点亮这个，熄灭其他的
    // 假设你的按钮设置了 checkable 属性
    ui->pushButton_friend_list_2->setChecked(true);

    // 如果有“消息列表”按钮，记得把它熄灭，防止两个按钮同时亮着
    // ui->pushButton_msg_list_2->setChecked(false);
}

void Client::on_pushBtn_send_2_clicked()
{
    // 1. 获取输入框 textEdit_send_2 的内容
    QString msg = ui->textEdit_send_2->toPlainText();

    if (msg.isEmpty() || m_chatTarget.isEmpty()) return;

    // 2. 显示在自己的界面上 (右对齐)
    QListWidgetItem *item = new QListWidgetItem(m_chatList);
    item->setText(msg);
    item->setTextAlignment(Qt::AlignRight); // 【关键】自己发的靠右
    m_chatList->addItem(item);
    m_chatList->scrollToBottom();

    // 3. 发送给服务器
    QJsonObject json;
    json["type"] = "chat";
    json["from"] = m_currentName;
    json["to"] = m_chatTarget;
    json["msg"] = msg;

    if(m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(QJsonDocument(json).toJson());
    }

    // 4. 清空输入框
    ui->textEdit_send_2->clear();
}

void Client::on_pushButton_addFriend_2_clicked()
{
    // 1. 弹窗输入好友名字
    bool ok;
    QString text = QInputDialog::getText(this, tr("添加好友"),
                                         tr("请输入对方用户名:"), QLineEdit::Normal,
                                         "", &ok);

    // 2. 如果点击了 OK 且输入不为空
    if (ok && !text.isEmpty()) {

        if(text == m_currentName) {
             QMessageBox::warning(this, "提示", "不能添加自己为好友！");
             return;
        }

        // 3. 发送请求给服务器
        QJsonObject json;
        json["type"] = "add_friend";
        json["user"] = m_currentName; // 我
        json["friend"] = text;        // 对方

        if(m_socket->state() == QAbstractSocket::ConnectedState) {
            m_socket->write(QJsonDocument(json).toJson());
        }
    }
}


void Client::addImageToChatList(const QPixmap& pix, bool isMe)
{
    // 1. 缩放图片，防止过大撑爆聊天框 (限制最大宽 200，高 200)
    QPixmap scaledPix = pix.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 2. 创建一个 Label 用来装图片
    QLabel *imgLabel = new QLabel();
    imgLabel->setPixmap(scaledPix);
    imgLabel->setStyleSheet("background-color: transparent;"); // 透明背景

    // 3. 创建列表项
    QListWidgetItem *item = new QListWidgetItem(m_chatList);

    // 4. 设置对齐方式
    if (isMe) {
        // 如果是自己发的，我们要想办法靠右 (Qt ListWidget 图片靠右比较麻烦，这里用个简单布局包裹一下)
        // 简单方案：直接往 item 里塞一个 widget，widget 里用 layout 把 label 挤到右边
        QWidget *container = new QWidget();
        QHBoxLayout *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0,0,0,0);
        layout->addStretch(); // 左边加弹簧
        layout->addWidget(imgLabel);

        item->setSizeHint(QSize(0, scaledPix.height() + 20)); // 设置高度
        m_chatList->setItemWidget(item, container);
    } else {
        // 别人发的，默认靠左
        // 为了统一，也用 container 包一层
        QWidget *container = new QWidget();
        QHBoxLayout *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0,0,0,0);
        layout->addWidget(imgLabel);
        layout->addStretch(); // 右边加弹簧

        item->setSizeHint(QSize(0, scaledPix.height() + 20));
        m_chatList->setItemWidget(item, container);
    }

    m_chatList->scrollToBottom();
}

void Client::on_pushButton_image_2_clicked()
{
    if (m_chatTarget.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择一个聊天对象");
        return;
    }

    // 1. 打开文件选择对话框
    QString fileName = QFileDialog::getOpenFileName(this, tr("选择图片"), "", tr("Images (*.png *.xpm *.jpg *.jpeg)"));

    if (fileName.isEmpty()) return;

    // 2. 加载图片
    QPixmap pix(fileName);
    if (pix.isNull()) return;

    // 3. 先在自己屏幕上显示
    addImageToChatList(pix, true); // true 代表是我发的

    // 4. 转成 Base64 字符串
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pix.save(&buffer, "PNG"); // 统一转成 PNG 格式发送
    QString base64Str = bytes.toBase64(); // 核心步骤：转码

    // 5. 打包 JSON 发送
    QJsonObject json;
    json["type"] = "img";
    json["from"] = m_currentName;
    json["to"] = m_chatTarget;
    json["data"] = base64Str; // 把长长的字符串塞进去

    if(m_socket->state() == QAbstractSocket::ConnectedState) {
        // 注意：如果图片太大（超过几MB），JSON包会很大，TCP可能会拆包导致接收失败。
        // 作为一个练习项目，建议发送小图。如果需要发大图，需要写更复杂的“分包/组包”逻辑。
        m_socket->write(QJsonDocument(json).toJson());
    }
}
