#include "livecamerawindow.h"

#include "desktopidle.h"
#include "uicommand.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>

namespace
{
QString cameraPreviewPath()
{
    const QByteArray value = qgetenv("XIAOMAN_CAMERA_PREVIEW_FILE");
    if (!value.isEmpty())
    {
        return QString::fromLocal8Bit(value);
    }
    const QByteArray legacyValue = qgetenv("NEWBOT_CAMERA_PREVIEW_FILE");
    if (!legacyValue.isEmpty())
    {
        return QString::fromLocal8Bit(legacyValue);
    }
    return QStringLiteral("/tmp/friday_voice_speaker_camera.jpg");
}

int previewIntervalMs()
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("XIAOMAN_CAMERA_PREVIEW_INTERVAL_MS", &ok);
    return std::max(80, ok ? value : 160);
}
} // namespace

LiveCameraWindow::LiveCameraWindow(const QString &title, const QString &command, QWidget *parent)
    : QWidget(parent),
      title_(title),
      command_(command),
      previewPath_(cameraPreviewPath()),
      imageLabel_(new QLabel(this)),
      statusLabel_(new QLabel(this)),
      stopButton_(new QPushButton(QStringLiteral("停止"), this)),
      timer_(new QTimer(this))
{
    setDesktopIdleReturnEnabled(true);
    setWindowFlags(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral(
        "LiveCameraWindow { background: #050506; }"
        "QLabel#statusLabel { color: #f5f7fb; background: rgba(0, 0, 0, 190);"
        " padding: 8px 14px; font-size: 17px; font-weight: 600; }"
        "QPushButton { color: #ffffff; background: rgba(255, 255, 255, 44);"
        " border: 1px solid rgba(255, 255, 255, 86); border-radius: 8px;"
        " padding: 10px 20px; font-size: 17px; font-weight: 700; }"
        "QPushButton:pressed { background: rgba(76, 214, 255, 120); }"));

    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLabel_->setText(title_ + QStringLiteral(" | 等待画面"));
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setStyleSheet(QStringLiteral("color: #dce7f3; font-size: 24px; background: #050506;"));
    imageLabel_->setText(QStringLiteral("等待摄像头画面"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(statusLabel_, 0);
    layout->addWidget(imageLabel_, 1);

    auto *toolbar = new QWidget(this);
    toolbar->setStyleSheet(QStringLiteral("background: rgba(0, 0, 0, 190);"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 8, 12, 8);
    toolbarLayout->setSpacing(8);
    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(stopButton_, 0);
    layout->addWidget(toolbar, 0);

    connect(stopButton_, &QPushButton::clicked, this, &LiveCameraWindow::stopAndReturn);
    connect(timer_, &QTimer::timeout, this, &LiveCameraWindow::refreshPreview);

    if (!command_.isEmpty())
    {
        sendUiCommand(command_);
    }
    timer_->start(previewIntervalMs());
    refreshPreview();
}

LiveCameraWindow::~LiveCameraWindow()
{
    if (!stopSent_ && !command_.isEmpty() && !property("_xiaomanSkipBackendStop").toBool())
    {
        sendUiCommand(QStringLiteral("s"));
    }
}

void LiveCameraWindow::setDisplayTitle(const QString &title)
{
    title_ = title;
    if (statusLabel_ != nullptr)
    {
        statusLabel_->setText(title_);
    }
}

void LiveCameraWindow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key())
    {
    case Qt::Key_Escape:
    case Qt::Key_F:
    case Qt::Key_S:
        stopAndReturn();
        event->accept();
        return;
    case Qt::Key_Q:
        stopSent_ = true;
        sendUiCommand(QStringLiteral("s"));
        qApp->quit();
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void LiveCameraWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    renderPixmap();
}

void LiveCameraWindow::refreshPreview()
{
    QFileInfo info(previewPath_);
    if (!info.exists() || info.size() <= 0)
    {
        statusLabel_->setText(title_ + QStringLiteral(" | 等待后端预览"));
        if (currentPixmap_.isNull())
        {
            imageLabel_->setText(QStringLiteral("等待摄像头画面"));
        }
        return;
    }

    QFile file(previewPath_);
    if (!file.open(QIODevice::ReadOnly))
    {
        statusLabel_->setText(title_ + QStringLiteral(" | 画面读取中"));
        return;
    }
    const QByteArray data = file.readAll();
    QPixmap pixmap;
    if (data.isEmpty() || !pixmap.loadFromData(data))
    {
        statusLabel_->setText(title_ + QStringLiteral(" | 画面解码中"));
        return;
    }

    currentPixmap_ = pixmap;
    lastModified_ = info.lastModified();
    statusLabel_->setText(title_);
    imageLabel_->setText(QString());
    renderPixmap();
}

void LiveCameraWindow::stopAndReturn()
{
    if (!stopSent_)
    {
        stopSent_ = true;
        sendUiCommand(QStringLiteral("s"));
    }
    showExpressionWindowAndCloseOthers();
}

void LiveCameraWindow::renderPixmap()
{
    if (currentPixmap_.isNull() || imageLabel_->size().isEmpty())
    {
        return;
    }
    imageLabel_->setPixmap(currentPixmap_.scaled(imageLabel_->size(),
                                                 Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation));
}
