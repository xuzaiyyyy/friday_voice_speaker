#include "expressionwindow.h"

#include "desktopidle.h"
#include "livecamerawindow.h"
#include "mainwindow.h"
#include "uicommand.h"
#include "apppaths.h"

#include <QApplication>
#include <QByteArray>
#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QDebug>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStringList>
#include <QTouchEvent>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace
{
int envInt(const char *name, int defaultValue)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? value : defaultValue;
}

QString envString(const char *name, const QString &defaultValue)
{
    const QByteArray value = qgetenv(name);
    return value.isEmpty() ? defaultValue : QString::fromLocal8Bit(value);
}

QStringList collectImages(const QString &directory, bool recursive)
{
    QStringList files;
    QDir dir(directory);
    if (!dir.exists())
    {
        return files;
    }

    const QStringList filters{"*.jpg", "*.jpeg", "*.png"};
    const QDirIterator::IteratorFlags flags =
        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator it(directory, filters, QDir::Files, flags);
    while (it.hasNext())
    {
        const QFileInfo fileInfo(it.next());
        if (fileInfo.size() > 0)
        {
            files.push_back(fileInfo.absoluteFilePath());
        }
    }

    QCollator collator;
    collator.setNumericMode(true);
    std::sort(files.begin(), files.end(), [&collator](const QString &left, const QString &right)
              { return collator.compare(left, right) < 0; });
    return files;
}

QStringList splitStates(const QString &text)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("，"), QStringLiteral(","));
    QStringList states;
    for (QString state : normalized.split(',', QString::SkipEmptyParts))
    {
        state = state.trimmed();
        if (!state.isEmpty() && !states.contains(state))
        {
            states << state;
        }
    }
    return states;
}

QStringList emojiRoots()
{
    return AppPaths::emojiRoots();
}

QString describeEmojiRoot(const QString &root)
{
    QDir dir(root);
    if (!dir.exists())
    {
        return QStringLiteral("missing");
    }
    const int dirCount = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot).size();
    const int fileCount = collectImages(root, true).size();
    return QStringLiteral("exists, dirs=%1, images=%2").arg(dirCount).arg(fileCount);
}

QImage decodeImageWithOpenCv(const QByteArray &data)
{
    if (data.isEmpty())
    {
        return QImage();
    }

    const auto *begin = reinterpret_cast<const unsigned char *>(data.constData());
    std::vector<unsigned char> encoded(begin, begin + data.size());
    const cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
    if (decoded.empty())
    {
        return QImage();
    }

    cv::Mat rgb;
    if (decoded.channels() == 4)
    {
        cv::cvtColor(decoded, rgb, cv::COLOR_BGRA2RGB);
    }
    else if (decoded.channels() == 3)
    {
        cv::cvtColor(decoded, rgb, cv::COLOR_BGR2RGB);
    }
    else if (decoded.channels() == 1)
    {
        cv::cvtColor(decoded, rgb, cv::COLOR_GRAY2RGB);
    }
    else
    {
        return QImage();
    }

    return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
}

void appendUniqueSet(QVector<QStringList> *sets, const QStringList &paths)
{
    if (paths.isEmpty())
    {
        return;
    }
    for (const QStringList &existing : *sets)
    {
        if (!existing.isEmpty() && !paths.isEmpty() && existing.first() == paths.first())
        {
            return;
        }
    }
    sets->push_back(paths);
}

QPushButton *buttonAt(QWidget *root, const QPoint &pos)
{
    QWidget *target = root->childAt(pos);
    while (target != nullptr && target != root)
    {
        if (auto *button = qobject_cast<QPushButton *>(target))
        {
            return button;
        }
        target = target->parentWidget();
    }
    return nullptr;
}

} // namespace

ExpressionWindow::ExpressionWindow(QWidget *parent)
    : QWidget(parent),
      imageLabel_(new QLabel(this)),
      timer_(new QTimer(this)),
      switchTimer_(new QTimer(this))
{
    setDesktopIdleReturnEnabled(false);
    setWindowFlags(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet("background: #000000;");

    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(imageLabel_, 1);

    auto *toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("commandToolbar"));
    toolbar->setStyleSheet(
        "#commandToolbar { background: rgba(0, 0, 0, 178); }"
        "QPushButton { color: #f7f7f7; background: rgba(255, 255, 255, 42);"
        " border: 1px solid rgba(255, 255, 255, 72); border-radius: 6px;"
        " padding: 7px 4px; font-size: 15px; font-weight: 600; }"
        "QPushButton:pressed { background: rgba(76, 214, 255, 110); }");
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(6, 6, 6, 6);
    toolbarLayout->setSpacing(5);
    auto *desktopButton = createToolbarButton(QStringLiteral("桌面"));
    connect(desktopButton, &QPushButton::clicked, this, &ExpressionWindow::enterDesktop);
    toolbarLayout->addWidget(desktopButton);
    addCommandButton(toolbarLayout, QStringLiteral("唤醒"), QStringLiteral("w"));
    addCommandButton(toolbarLayout, QStringLiteral("视觉"), QStringLiteral("v"));
    addLiveCameraButton(toolbarLayout, QStringLiteral("摄像头"), QStringLiteral("c"));
    addLiveCameraButton(toolbarLayout, QStringLiteral("情感"), QStringLiteral("e"));
    addCommandButton(toolbarLayout, QStringLiteral("停止"), QStringLiteral("s"));
    addCommandButton(toolbarLayout, QStringLiteral("音乐"), QStringLiteral("来点音乐"));
    auto *exitButton = createToolbarButton(QStringLiteral("退出"));
    connect(exitButton, &QPushButton::clicked, this, []
            { qApp->quit(); });
    toolbarLayout->addWidget(exitButton);
    layout->addWidget(toolbar, 0);

    discoverEmojiSets();
    connect(timer_, &QTimer::timeout, this, &ExpressionWindow::showNextFrame);
    timer_->start(std::max(30, envInt("XIAOMAN_DESKTOP_EMOJI_INTERVAL_MS", 70)));
    connect(switchTimer_, &QTimer::timeout, this, &ExpressionWindow::switchEmojiSet);
    switchTimer_->start(std::max(1, envInt("XIAOMAN_DESKTOP_EMOJI_SWITCH_SECONDS", 8)) * 1000);
    renderCurrentFrame();
}

QPushButton *ExpressionWindow::createToolbarButton(const QString &label)
{
    auto *button = new QPushButton(label, this);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumHeight(42);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return button;
}

void ExpressionWindow::addCommandButton(QHBoxLayout *layout, const QString &label, const QString &command)
{
    auto *button = createToolbarButton(label);
    connect(button, &QPushButton::clicked, this, [command]
            { sendUiCommand(command); });
    layout->addWidget(button);
}

void ExpressionWindow::addLiveCameraButton(QHBoxLayout *layout, const QString &label, const QString &command)
{
    auto *button = createToolbarButton(label);
    connect(button, &QPushButton::clicked, this, [this, label, command]
            { openLiveCamera(label, command); });
    layout->addWidget(button);
}

bool ExpressionWindow::event(QEvent *event)
{
    if (event->type() == QEvent::TouchBegin)
    {
        auto *touch = static_cast<QTouchEvent *>(event);
        if (!touch->touchPoints().isEmpty())
        {
            pressPos_ = touch->touchPoints().first().pos().toPoint();
            lastTouchPos_ = pressPos_;
        }
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchUpdate)
    {
        auto *touch = static_cast<QTouchEvent *>(event);
        if (!touch->touchPoints().isEmpty())
        {
            lastTouchPos_ = touch->touchPoints().first().pos().toPoint();
        }
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchEnd)
    {
        auto *touch = static_cast<QTouchEvent *>(event);
        QPoint releasePos = lastTouchPos_;
        if (!touch->touchPoints().isEmpty())
        {
            releasePos = touch->touchPoints().first().pos().toPoint();
        }
        const QPoint delta = releasePos - pressPos_;
        const int threshold = std::max(48, width() / 10);
        QPushButton *pressedButton = buttonAt(this, pressPos_);
        QPushButton *releasedButton = buttonAt(this, releasePos);
        if (pressedButton != nullptr && pressedButton == releasedButton &&
            std::abs(delta.x()) < threshold && std::abs(delta.y()) < threshold)
        {
            pressedButton->click();
            event->accept();
            return true;
        }
        if (pressedButton != nullptr || releasedButton != nullptr)
        {
            event->accept();
            return true;
        }
        if (std::abs(delta.x()) >= threshold || std::abs(delta.y()) >= threshold)
        {
            enterDesktop();
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void ExpressionWindow::keyPressEvent(QKeyEvent *event)
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
        openLiveCamera(QStringLiteral("摄像头"), QStringLiteral("c"));
        event->accept();
        return;
    case Qt::Key_F:
        event->accept();
        return;
    case Qt::Key_E:
        openLiveCamera(QStringLiteral("情感"), QStringLiteral("e"));
        event->accept();
        return;
    case Qt::Key_V:
        command = QStringLiteral("v");
        break;
    case Qt::Key_S:
        command = QStringLiteral("s");
        break;
    case Qt::Key_M:
        command = QStringLiteral("m");
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        enterDesktop();
        event->accept();
        return;
    default:
        break;
    }

    if (!command.isEmpty())
    {
        sendUiCommand(command);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ExpressionWindow::mousePressEvent(QMouseEvent *event)
{
    pressPos_ = event->pos();
    QWidget::mousePressEvent(event);
}

void ExpressionWindow::mouseReleaseEvent(QMouseEvent *event)
{
    const QPoint delta = event->pos() - pressPos_;
    const int threshold = std::max(48, width() / 10);
    if (buttonAt(this, pressPos_) != nullptr || buttonAt(this, event->pos()) != nullptr)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (std::abs(delta.x()) >= threshold || std::abs(delta.y()) >= threshold)
    {
        enterDesktop();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ExpressionWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    renderCurrentFrame();
}

void ExpressionWindow::showNextFrame()
{
    if (frames_.isEmpty())
    {
        return;
    }
    frameIndex_ = (frameIndex_ + 1) % frames_.size();
    renderCurrentFrame();
}

void ExpressionWindow::switchEmojiSet()
{
    if (emojiSets_.size() <= 1)
    {
        return;
    }
    emojiSetIndex_ = (emojiSetIndex_ + 1) % emojiSets_.size();
    loadFramesFromPaths(emojiSets_[emojiSetIndex_]);
    renderCurrentFrame();
}

void ExpressionWindow::discoverEmojiSets()
{
    emojiSets_.clear();

    QString statesText = envString("XIAOMAN_DESKTOP_EMOJI_STATES", QString());
    const QString singleState = envString("XIAOMAN_DESKTOP_EMOJI_STATE", QString());
    if (!singleState.isEmpty())
    {
        statesText = singleState + QStringLiteral(",") + statesText;
    }
    const QStringList requestedStates = splitStates(statesText);
    const QStringList roots = emojiRoots();

    for (const QString &root : roots)
    {
        qInfo() << "expression window: emoji root" << root << describeEmojiRoot(root);
    }

    for (const QString &root : roots)
    {
        QDir rootDir(root);
        if (!rootDir.exists())
        {
            continue;
        }

        if (!requestedStates.isEmpty())
        {
            for (const QString &state : requestedStates)
            {
                appendUniqueSet(&emojiSets_, collectImages(rootDir.filePath(state), true));
            }
        }
        else
        {
            QVector<QStringList> discovered;
            const QFileInfoList dirs = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo &dirInfo : dirs)
            {
                appendUniqueSet(&discovered, collectImages(dirInfo.absoluteFilePath(), true));
            }
            std::sort(discovered.begin(), discovered.end(), [](const QStringList &left, const QStringList &right)
                      { return left.size() > right.size(); });
            for (const QStringList &paths : discovered)
            {
                appendUniqueSet(&emojiSets_, paths);
            }
        }
        if (!emojiSets_.isEmpty())
        {
            break;
        }
    }

    if (emojiSets_.isEmpty())
    {
        for (const QString &root : roots)
        {
            appendUniqueSet(&emojiSets_, collectImages(root, true));
            if (!emojiSets_.isEmpty())
            {
                break;
            }
        }
    }

    if (!emojiSets_.isEmpty())
    {
        emojiSetIndex_ = 0;
        qInfo() << "expression window: loaded" << emojiSets_.size()
                << "emoji sets, first set frames" << emojiSets_.front().size();
        loadFramesFromPaths(emojiSets_.front());
        return;
    }

    qWarning() << "expression window: no emoji images found. Set XIAOMAN_DESKTOP_EMOJI_ROOT or run from desktop/ beside ../image";
    imageLabel_->setText(QStringLiteral("FRIDAY"));
    imageLabel_->setStyleSheet("color: white; font-size: 42px; font-weight: 700;");
}

void ExpressionWindow::loadFramesFromPaths(const QStringList &imagePaths)
{
    frames_.clear();
    frameIndex_ = 0;

    const int maxFrames = std::max(1, envInt("XIAOMAN_DESKTOP_EMOJI_MAX_FRAMES", 180));
    int failedReads = 0;
    for (const QString &path : imagePaths)
    {
        QFile file(path);
        QImage image;
        QByteArray data;
        QString errorText;
        if (file.open(QIODevice::ReadOnly))
        {
            data = file.readAll();
            if (!image.loadFromData(data))
            {
                image = decodeImageWithOpenCv(data);
            }
            if (image.isNull())
            {
                QImageReader reader(path);
                image = reader.read();
                errorText = reader.errorString();
            }
        }
        else
        {
            errorText = file.errorString();
        }

        if (!image.isNull())
        {
            frames_.push_back(QPixmap::fromImage(image));
            if (frames_.size() >= maxFrames)
            {
                break;
            }
        }
        else if (failedReads < 3)
        {
            ++failedReads;
            const QByteArray header = data.left(12).toHex(' ');
            qWarning() << "expression window: failed to decode emoji frame"
                       << path << errorText
                       << "bytes" << data.size()
                       << "header" << header;
        }
    }

    qInfo() << "expression window: decoded" << frames_.size()
            << "frames from" << imagePaths.size() << "image paths";

    if (frames_.isEmpty())
    {
        qWarning() << "expression window: no decodable emoji frames. Qt image formats:"
                   << QImageReader::supportedImageFormats();
        imageLabel_->setText(QStringLiteral("FRIDAY"));
        imageLabel_->setStyleSheet("color: white; font-size: 42px; font-weight: 700;");
        return;
    }
    imageLabel_->setText(QString());
    imageLabel_->setStyleSheet(QString());
}

void ExpressionWindow::renderCurrentFrame()
{
    if (frames_.isEmpty() || imageLabel_->size().isEmpty())
    {
        return;
    }
    const QPixmap &source = frames_[frameIndex_ % frames_.size()];
    imageLabel_->setPixmap(source.scaled(imageLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ExpressionWindow::enterDesktop()
{
    if (desktopEntered_)
    {
        return;
    }
    desktopEntered_ = true;
    auto *window = new MainWindow();
    replaceDesktopTopLevel(this, window);
}

void ExpressionWindow::openLiveCamera(const QString &title, const QString &command)
{
    if (desktopEntered_)
    {
        return;
    }
    desktopEntered_ = true;
    auto *window = new LiveCameraWindow(title, command);
    replaceDesktopTopLevel(this, window);
}
