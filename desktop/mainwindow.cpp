#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "desktopidle.h"
#include "uicommand.h"

#include <QApplication>
#include <QList>
#include <QPushButton>
#include <QResizeEvent>
#include <QtGlobal>

namespace
{
template <typename Page>
void switchToPage(QWidget *current)
{
    auto *page = new Page();
    replaceDesktopTopLevel(current, page);
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setDesktopIdleReturnEnabled(true);
    m_isPressed = false;

    //创建并设置定时器
    m_timer = new QTimer(this);
    m_timer->setInterval(1000); // 1秒
    connect(m_timer, &QTimer::timeout, this, &MainWindow::updateTime);
    m_timer->start();
    updateTime();



    this->setWindowFlags(Qt::FramelessWindowHint);
    this->setAttribute(Qt::WA_TranslucentBackground);

    // 阴影设置保持不变
    QGraphicsDropShadowEffect * shadowEffect = new QGraphicsDropShadowEffect();
    shadowEffect->setOffset(0, 0);
    shadowEffect->setColor(QColor(0, 0, 0, 64));
    shadowEffect->setBlurRadius(20);
    ui->frame->setGraphicsEffect(shadowEffect);

    // 【关键修改 1】在这里直接创建动画对象，并指定父对象为 this (自动管理内存)
    // 以后只操作这个对象，不反复 new 和 delete
    m_animation = new QPropertyAnimation(ui->frame, "pos", this);
    m_animation->setDuration(300);
    m_animation->setEasingCurve(QEasingCurve::OutQuad);

    const QList<QWidget *> frameChildren =
        ui->frame->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *child : frameChildren)
    {
        m_originalFrameGeometry.insert(child, child->geometry());
    }
    updateResponsiveLayout();
}

MainWindow::~MainWindow()
{
    delete ui;
}


void MainWindow::on_btn_photo_clicked()
{
    switchToPage<photo>(this);
}

void MainWindow::on_btn_calculate_clicked()
{
    switchToPage<calculate>(this);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    QString command;
    switch (event->key())
    {
    case Qt::Key_Escape:
    case Qt::Key_Q:
        qApp->quit();
        event->accept();
        return;
    case Qt::Key_W:
        command = QStringLiteral("w");
        break;
    case Qt::Key_C:
        command = QStringLiteral("c");
        break;
    case Qt::Key_F:
        showExpressionWindowAndCloseOthers();
        event->accept();
        return;
    case Qt::Key_E:
        command = QStringLiteral("e");
        break;
    case Qt::Key_V:
        command = QStringLiteral("v");
        break;
    case Qt::Key_S:
        command = QStringLiteral("s");
        break;
    case Qt::Key_M:
        command = QStringLiteral("m");
        break;
    default:
        break;
    }

    if (!command.isEmpty())
    {
        sendUiCommand(command);
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::mousePressEvent(QMouseEvent * event)
{
    if(event->button() == Qt::LeftButton)
    {
        // 【关键修改 2】安全停止
        // 因为我们不再使用 DeleteWhenStopped，所以 stop() 只会暂停动画
        // 指针依然是安全的，不会变成野指针
        if(m_animation->state() == QAbstractAnimation::Running)
        {
            m_animation->stop();
        }

        m_isPressed = true;
        m_offset = event->pos() - ui->frame->pos();
    }
}


void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if(m_isPressed)
    {
        int newX = event->pos().x() - m_offset.x();

        // 限制在当前屏幕宽度的一页范围内
        int safeX = qBound(-width(), newX, 0);

        ui->frame->move(safeX, ui->frame->y());
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event) {
    m_isPressed = false;

    // 计算目标位置
    int targetX = (ui->frame->x() < -width() / 2) ? -width() : 0;

    // 【关键修改 3】不要 new 新的，直接重置旧的
    m_animation->setStartValue(ui->frame->pos()); // 从当前松手的位置开始
    m_animation->setEndValue(QPoint(targetX, ui->frame->y())); // 到目标位置

    // 【绝对禁止】这里千万不要加 QAbstractAnimation::DeleteWhenStopped
    m_animation->start();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateResponsiveLayout();
}

void MainWindow::updateResponsiveLayout()
{
    const int pageWidth = width();
    const int pageHeight = height();
    if (pageWidth <= 0 || pageHeight <= 0 || m_originalFrameGeometry.isEmpty())
    {
        return;
    }

    const int currentPage = ui->frame->x() < -pageWidth / 2 ? 1 : 0;
    ui->centralwidget->resize(pageWidth, pageHeight);
    ui->frame->setGeometry(-currentPage * pageWidth, 0, pageWidth * 2, pageHeight);

    const qreal sx = static_cast<qreal>(pageWidth) / 1024.0;
    const qreal sy = static_cast<qreal>(pageHeight) / 600.0;
    const qreal sizeScale = qMin(sx, sy);

    for (auto it = m_originalFrameGeometry.constBegin(); it != m_originalFrameGeometry.constEnd(); ++it)
    {
        QWidget *child = it.key();
        if (child == nullptr)
        {
            continue;
        }
        const QRect original = it.value();
        const int page = original.x() >= 1024 ? 1 : 0;
        const int localX = original.x() - page * 1024;
        const QRect scaled(page * pageWidth + qRound(localX * sx),
                           qRound(original.y() * sy),
                           qMax(24, qRound(original.width() * sizeScale)),
                           qMax(20, qRound(original.height() * sizeScale)));
        child->setGeometry(scaled);
    }

    const int shortSide = qMin(pageWidth, pageHeight);
    const int margin = qBound(8, shortSide / 36, 14);
    const int dockButtonSize = qBound(44, shortSide / 8, 62);
    const int dockGap = qBound(6, dockButtonSize / 5, 12);
    const int dockHeight = dockButtonSize + margin * 2;
    const QList<QPushButton *> dockButtons = {
        ui->btn_weather,
        ui->btn_music,
        ui->btn_printbrush,
        ui->btn_fileview,
        ui->btn_close};
    const int dockButtonCount = dockButtons.size();
    const int dockWidth = qMin(pageWidth - margin * 2,
                               dockButtonSize * dockButtonCount + dockGap * (dockButtonCount + 1));
    const int dockX = (pageWidth - dockWidth) / 2;
    const int dockY = qMax(margin, pageHeight - dockHeight - margin);

    ui->frame_dock->setGeometry(dockX, dockY, dockWidth, dockHeight);
    ui->frame_dock->setStyleSheet(QStringLiteral(
        "#frame_dock { background-color: rgba(255, 255, 255, 58);"
        " border: 1px solid rgba(255, 255, 255, 96); border-radius: %1px; }")
                                      .arg(dockHeight / 2));
    ui->frame_dock->raise();

    int buttonX = (dockWidth - dockButtonSize * dockButtonCount - dockGap * (dockButtonCount - 1)) / 2;
    for (QPushButton *button : dockButtons)
    {
        if (button == nullptr)
        {
            continue;
        }
        button->setGeometry(buttonX, margin, dockButtonSize, dockButtonSize);
        button->setMinimumSize(dockButtonSize, dockButtonSize);
        button->setMaximumSize(dockButtonSize, dockButtonSize);
        button->raise();
        buttonX += dockButtonSize + dockGap;
    }

    const int timeWidth = qMin(pageWidth - margin * 2, qMax(160, pageWidth / 3));
    const int timeHeight = qBound(24, pageHeight / 18, 34);
    ui->xinxi->setGeometry(pageWidth - margin - timeWidth, margin, timeWidth, timeHeight);
    ui->xinxi->raise();
}

void MainWindow::on_btn_camera_clicked()
{
    switchToPage<CameraPage>(this);
}

void MainWindow::updateTime()
{
    //获取当前系统时间
    QDateTime current = QDateTime::currentDateTime();
    QString timeStr = current.toString("MM月dd日 ddd HH:mm");
    ui->xinxi->setText(timeStr);
}

void MainWindow::on_btn_close_clicked()
{
    showExpressionWindowAndCloseOthers();
}

void MainWindow::on_btn_printbrush_clicked()
{
    switchToPage<sketchpad>(this);
}

void MainWindow::on_btn_server_clicked()
{
    switchToPage<ServerPage>(this);
}

void MainWindow::on_btn_netcamera_clicked()
{
    switchToPage<NetCameraPage>(this);
}

void MainWindow::on_btn_weather_clicked()
{
    switchToPage<WeatherPage>(this);
}

void MainWindow::on_btn_music_clicked()
{
    switchToPage<MusicPage>(this);
}

void MainWindow::on_btn_fileview_clicked()
{
    switchToPage<FileManager>(this);
}

void MainWindow::on_btn_wechat_clicked()
{
    switchToPage<ChatClient>(this);
}

void MainWindow::on_btn_door_clicked()
{
    switchToPage<Door>(this);
}
