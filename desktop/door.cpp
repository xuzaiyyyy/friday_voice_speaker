#include "door.h"
#include "ui_door.h"
#include "desktopidle.h"
#include <QDateTime>
#include <QFont>
#include <QGridLayout>
#include <QResizeEvent>
#include <QStringList>
#include <QtGlobal>
#include <QVBoxLayout>
#include <QHBoxLayout>

// 定义管理员密码 (您可以修改这个)
const QString ADMIN_PASSWORD = "********";

Door::Door(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Door)
{
    ui->setupUi(this);
    this->setWindowFlags(Qt::FramelessWindowHint);
    m_inputStep = InputStep::None;

    // ==========================================
    // 1. 初始化视频显示区域 (放入布局)
    // ==========================================
    m_displayLabel = new QLabel(ui->videoArea);
    m_displayLabel->setAlignment(Qt::AlignCenter);
    m_displayLabel->setStyleSheet("background-color: black;");
    m_displayLabel->setScaledContents(true);

    QHBoxLayout *layout = new QHBoxLayout(ui->videoArea);
    layout->setMargin(0);
    layout->addWidget(m_displayLabel);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setGeometry(12, 8, 224, 44);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet(
        "QLabel { color: white; background: rgba(0,0,0,150); "
        "border-radius: 8px; font-size: 16px; font-weight: bold; padding: 4px; }");
    setStatusText("猫眼已启动");

    setupInlineInputPanel();
    updateOverlayGeometry();

    // ==========================================
    // 2. 初始化 AI 线程
    // ==========================================
    m_faceThread = new FaceThread(this);
    connect(m_faceThread, &FaceThread::frameProcessed, this, &Door::updateImage);
    connect(m_faceThread, &FaceThread::logMessage, this, &Door::updateLog);

    // ==========================================
    // 3. 启动 AI
    // ==========================================
    m_faceThread->start();
}

Door::~Door()
{
    if (m_faceThread->isRunning()) {
        m_faceThread->stop();
        m_faceThread->wait();
    }
    delete ui;
}

void Door::updateImage(QImage image)
{
    m_displayLabel->setPixmap(QPixmap::fromImage(image));
}

void Door::updateLog(QString msg)
{
    qDebug() << "[AI Log]:" << msg;
    if (msg.contains("录入完成") || msg.contains("成功删除") || msg.contains("未找到") ||
        msg.contains("数据库") || msg.contains("错误") || msg.contains("警告")) {
        setStatusText(msg);
    }
}

void Door::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateOverlayGeometry();
}

void Door::updateOverlayGeometry()
{
    if (m_statusLabel) {
        const int sideWidth = qMax(160, ui->videoArea->geometry().x() - 24);
        m_statusLabel->setGeometry(12, 8, qMin(224, sideWidth), 40);
    }

    if (!m_inputPanel) {
        return;
    }

    QRect target = ui->videoArea->geometry();
    if (target.width() < 260 || target.height() < 220) {
        target = rect().adjusted(12, 12, -12, -12);
    }

    const int panelW = qMin(420, qMax(300, target.width() - 24));
    const int panelH = qMin(318, qMax(270, target.height() - 24));
    const QRect bounds = rect().adjusted(8, 8, -8, -8);
    const int rawX = target.x() + (target.width() - panelW) / 2;
    const int rawY = target.y() + (target.height() - panelH) / 2;
    const int x = qBound(bounds.left(), rawX, bounds.right() - panelW + 1);
    const int y = qBound(bounds.top(), rawY, bounds.bottom() - panelH + 1);
    m_inputPanel->setGeometry(x, y, panelW, panelH);
}

void Door::setStatusText(const QString &text)
{
    if (m_statusLabel) {
        m_statusLabel->setText(text);
    }
}

QString Door::defaultFaceName() const
{
    return QStringLiteral("user_") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
}

void Door::setupInlineInputPanel()
{
    m_inputPanel = new QFrame(this);
    m_inputPanel->setStyleSheet(
        "QFrame { background: rgba(12,18,28,230); border: 2px solid rgba(255,255,255,130); border-radius: 12px; }"
        "QLabel { color: white; background: transparent; border: none; }"
        "QLineEdit { min-height: 34px; color: white; background: rgba(255,255,255,35); "
        "border: 1px solid rgba(255,255,255,120); border-radius: 6px; font-size: 18px; padding: 2px 8px; }"
        "QPushButton { min-height: 34px; color: white; background: rgba(80,145,230,210); "
        "border: 1px solid rgba(255,255,255,120); border-radius: 7px; font-size: 16px; font-weight: bold; }"
        "QPushButton:pressed { background: rgba(30,110,210,230); }");

    auto *outer = new QVBoxLayout(m_inputPanel);
    outer->setContentsMargins(14, 10, 14, 10);
    outer->setSpacing(6);

    m_inputTitle = new QLabel(m_inputPanel);
    m_inputTitle->setAlignment(Qt::AlignCenter);
    QFont titleFont = m_inputTitle->font();
    titleFont.setPixelSize(20);
    titleFont.setBold(true);
    m_inputTitle->setFont(titleFont);
    outer->addWidget(m_inputTitle);

    m_inputHint = new QLabel(m_inputPanel);
    m_inputHint->setWordWrap(true);
    m_inputHint->setAlignment(Qt::AlignCenter);
    QFont hintFont = m_inputHint->font();
    hintFont.setPixelSize(14);
    m_inputHint->setFont(hintFont);
    outer->addWidget(m_inputHint);

    m_inputEdit = new QLineEdit(m_inputPanel);
    m_inputEdit->setAlignment(Qt::AlignCenter);
    outer->addWidget(m_inputEdit);

    m_digitPad = new QWidget(m_inputPanel);
    auto *digitLayout = new QGridLayout(m_digitPad);
    digitLayout->setContentsMargins(0, 0, 0, 0);
    digitLayout->setHorizontalSpacing(6);
    digitLayout->setVerticalSpacing(5);
    const QStringList keys = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "清", "0", "退"};
    for (int i = 0; i < keys.size(); ++i) {
        auto *key = new QPushButton(keys[i], m_digitPad);
        key->setMinimumHeight(30);
        key->setMaximumHeight(34);
        digitLayout->addWidget(key, i / 3, i % 3);
        connect(key, &QPushButton::clicked, this, [this, key]() {
            const QString text = key->text();
            if (text == "清") {
                m_inputEdit->clear();
            } else if (text == "退") {
                m_inputEdit->backspace();
            } else {
                m_inputEdit->insert(text);
            }
        });
    }
    outer->addWidget(m_digitPad);

    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(12);
    m_inputCancel = new QPushButton("取消", m_inputPanel);
    m_inputConfirm = new QPushButton("确认", m_inputPanel);
    buttons->addWidget(m_inputCancel);
    buttons->addWidget(m_inputConfirm);
    outer->addLayout(buttons);

    connect(m_inputConfirm, &QPushButton::clicked, this, &Door::confirmInlineInput);
    connect(m_inputCancel, &QPushButton::clicked, this, &Door::cancelInlineInput);
    connect(m_inputEdit, &QLineEdit::returnPressed, this, &Door::confirmInlineInput);

    m_inputPanel->hide();
}

void Door::showInlineInput(InputStep step)
{
    updateOverlayGeometry();
    m_inputStep = step;
    m_inputEdit->clear();
    m_inputEdit->setEchoMode(QLineEdit::Normal);
    m_inputEdit->setInputMethodHints(Qt::ImhNone);
    m_digitPad->hide();

    if (step == InputStep::RegisterPassword || step == InputStep::DeletePassword) {
        m_inputTitle->setText(step == InputStep::RegisterPassword ? "录入人脸" : "删除记录");
        m_inputHint->setText("输入管理员密码后点确认");
        m_inputEdit->setEchoMode(QLineEdit::Password);
        m_inputEdit->setInputMethodHints(Qt::ImhDigitsOnly);
        m_digitPad->show();
    } else if (step == InputStep::RegisterName) {
        m_inputTitle->setText("录入姓名");
        m_inputHint->setText("可直接使用默认名称");
        m_inputEdit->setText(defaultFaceName());
        m_inputEdit->selectAll();
    } else if (step == InputStep::DeleteName) {
        m_inputTitle->setText("删除记录");
        m_inputHint->setText("请输入要删除的人脸姓名");
    }

    if (m_statusLabel) {
        m_statusLabel->hide();
    }
    m_inputPanel->raise();
    m_inputPanel->show();
    m_inputEdit->setFocus(Qt::OtherFocusReason);
}

void Door::hideInlineInput()
{
    m_inputStep = InputStep::None;
    m_inputEdit->clear();
    m_inputPanel->hide();
    if (m_statusLabel) {
        m_statusLabel->show();
    }
}

void Door::confirmInlineInput()
{
    const QString value = m_inputEdit->text().trimmed();

    switch (m_inputStep) {
    case InputStep::RegisterPassword:
        if (value != ADMIN_PASSWORD) {
            setStatusText("密码错误");
            hideInlineInput();
            return;
        }
        showInlineInput(InputStep::RegisterName);
        return;
    case InputStep::RegisterName:
        if (value.isEmpty()) {
            setStatusText("姓名不能为空");
            return;
        }
        m_faceThread->registerFace(value);
        setStatusText(QString("正在录入 %1，请注视摄像头").arg(value));
        hideInlineInput();
        return;
    case InputStep::DeletePassword:
        if (value != ADMIN_PASSWORD) {
            setStatusText("密码错误");
            hideInlineInput();
            return;
        }
        showInlineInput(InputStep::DeleteName);
        return;
    case InputStep::DeleteName:
        if (value.isEmpty()) {
            setStatusText("删除姓名不能为空");
            return;
        }
        m_faceThread->deleteRecord(value);
        setStatusText(QString("已发送删除指令: %1").arg(value));
        hideInlineInput();
        return;
    case InputStep::None:
        return;
    }
}

void Door::cancelInlineInput()
{
    hideInlineInput();
    setStatusText("操作已取消");
}

// 按钮1：录入人脸 (带密码 + 姓名)
void Door::on_pushButton_clicked()
{
    showInlineInput(InputStep::RegisterPassword);
}

// 按钮2：开始识别
void Door::on_pushButton_2_clicked()
{
    m_faceThread->startRecognition();
    setStatusText("识别模式已开启");
    ui->pushButton_2->setEnabled(false); // 开始变灰
    ui->pushButton_4->setEnabled(true);  // 停止变亮
}

// 按钮3：删除记录 (带密码 + 指定姓名)
void Door::on_pushButton_3_clicked()
{
    showInlineInput(InputStep::DeletePassword);
}

// 按钮4：停止识别
void Door::on_pushButton_4_clicked()
{
    m_faceThread->stopRecognition();
    setStatusText("识别已停止");
    ui->pushButton_2->setEnabled(true);
    ui->pushButton_4->setEnabled(false);
}

void Door::on_photo_back_clicked()
{
    // ============================================
    // 1. 核心修复：手动停止 AI 线程并释放摄像头
    // ============================================
    if (m_faceThread != nullptr && m_faceThread->isRunning()) {
        // 告诉线程停止循环
        m_faceThread->stop();
        // 【关键】阻塞等待线程彻底结束！
        // 这一步会确保 run() 函数执行完，cap.release() 被调用
        m_faceThread->wait();
    }
    this->setAttribute(Qt::WA_DeleteOnClose);
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}
