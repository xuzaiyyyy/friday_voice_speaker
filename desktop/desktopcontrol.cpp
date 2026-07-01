#include "desktopcontrol.h"

#include "desktopidle.h"
#include "livecamerawindow.h"

#include <QApplication>
#include <QDebug>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <QWidget>

#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
QString desktopControlPath()
{
    QByteArray path = qgetenv("XIAOMAN_DESKTOP_CONTROL_FIFO");
    if (path.isEmpty())
    {
        path = qgetenv("NEWBOT_DESKTOP_CONTROL_FIFO");
    }
    if (path.isEmpty())
    {
        path = "/tmp/friday_voice_speaker_desktop.fifo";
    }
    return QString::fromLocal8Bit(path);
}

bool matchesAny(const QString &value, std::initializer_list<QString> items)
{
    for (const QString &item : items)
    {
        if (value == item)
        {
            return true;
        }
    }
    return false;
}

void showLiveCameraFromBackend(const QString &title)
{
    const QList<QWidget *> existingWindows = QApplication::topLevelWidgets();
    for (QWidget *existingWindow : existingWindows)
    {
        auto *liveWindow = qobject_cast<LiveCameraWindow *>(existingWindow);
        if (liveWindow != nullptr && liveWindow->isVisible())
        {
            liveWindow->setDisplayTitle(title);
            liveWindow->raise();
            liveWindow->activateWindow();
            return;
        }
    }

    auto *window = new LiveCameraWindow(title, QString());
    window->setAttribute(Qt::WA_DeleteOnClose, true);
    showDesktopTopLevel(window);

    const QList<QWidget *> windows = QApplication::topLevelWidgets();
    QList<QPointer<QWidget>> oldWindows;
    for (QWidget *oldWindow : windows)
    {
        if (oldWindow != nullptr && oldWindow != window)
        {
            if (QString::fromLatin1(oldWindow->metaObject()->className()) == QStringLiteral("LiveCameraWindow"))
            {
                oldWindow->setProperty("_xiaomanSkipBackendStop", true);
            }
            oldWindows.push_back(QPointer<QWidget>(oldWindow));
        }
    }
    QTimer::singleShot(80, window, [oldWindows, window]() {
        for (const QPointer<QWidget> &oldWindow : oldWindows)
        {
            if (oldWindow && oldWindow != window)
            {
                oldWindow->close();
            }
        }
    });
}
} // namespace

DesktopControlPipe::DesktopControlPipe(QObject *parent)
    : QObject(parent),
      path_(desktopControlPath())
{
    if (!ensureFifo())
    {
        return;
    }

    QByteArray localPath = path_.toLocal8Bit();
    fd_ = ::open(localPath.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0 && errno == EACCES)
    {
        fd_ = ::open(localPath.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd_ < 0)
    {
        qWarning() << "desktop control fifo open failed:" << path_ << strerror(errno);
        return;
    }

    notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this]() {
        readAvailable();
    });
    qInfo() << "desktop control fifo=" << path_;
}

DesktopControlPipe::~DesktopControlPipe()
{
    if (notifier_)
    {
        notifier_->setEnabled(false);
    }
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
}

bool DesktopControlPipe::ensureFifo()
{
    QByteArray localPath = path_.toLocal8Bit();
    struct stat st
    {
    };
    if (::stat(localPath.constData(), &st) == 0)
    {
        if (!S_ISFIFO(st.st_mode))
        {
            qWarning() << "desktop control path exists but is not fifo:" << path_;
            return false;
        }
        if (::chmod(localPath.constData(), 0666) != 0)
        {
            qWarning() << "desktop control fifo chmod failed:" << path_ << strerror(errno);
        }
        return true;
    }
    if (errno != ENOENT)
    {
        qWarning() << "desktop control fifo stat failed:" << path_ << strerror(errno);
        return false;
    }
    if (::mkfifo(localPath.constData(), 0666) != 0 && errno != EEXIST)
    {
        qWarning() << "desktop control fifo mkfifo failed:" << path_ << strerror(errno);
        return false;
    }
    if (::chmod(localPath.constData(), 0666) != 0)
    {
        qWarning() << "desktop control fifo chmod failed:" << path_ << strerror(errno);
    }
    return true;
}

void DesktopControlPipe::readAvailable()
{
    if (fd_ < 0)
    {
        return;
    }

    char buffer[256];
    for (;;)
    {
        const ssize_t n = ::read(fd_, buffer, sizeof(buffer));
        if (n > 0)
        {
            pending_.append(buffer, static_cast<int>(n));
            for (;;)
            {
                const int pos = pending_.indexOf('\n');
                if (pos < 0)
                {
                    break;
                }
                QByteArray line = pending_.left(pos);
                pending_.remove(0, pos + 1);
                if (line.endsWith('\r'))
                {
                    line.chop(1);
                }
                handleLine(QString::fromUtf8(line).trimmed());
            }
            if (pending_.size() > 4096)
            {
                pending_.clear();
            }
            continue;
        }
        if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return;
        }
        qWarning() << "desktop control fifo read failed:" << path_ << strerror(errno);
        return;
    }
}

void DesktopControlPipe::handleLine(const QString &line)
{
    const QString command = line.trimmed().toLower();
    if (command.isEmpty())
    {
        return;
    }

    if (matchesAny(command, {QStringLiteral("camera"),
                             QStringLiteral("open_camera"),
                             QStringLiteral("c"),
                             QStringLiteral("摄像头"),
                             QStringLiteral("打开摄像头")}))
    {
        showLiveCameraFromBackend(QStringLiteral("摄像头"));
        return;
    }

    if (matchesAny(command, {QStringLiteral("emotion"),
                             QStringLiteral("start_emotion"),
                             QStringLiteral("e"),
                             QStringLiteral("情感"),
                             QStringLiteral("情感识别"),
                             QStringLiteral("情感检测"),
                             QStringLiteral("开始情感识别"),
                             QStringLiteral("开始情感检测")}))
    {
        showLiveCameraFromBackend(QStringLiteral("情感"));
        return;
    }

    if (matchesAny(command, {QStringLiteral("expression"),
                             QStringLiteral("emoji"),
                             QStringLiteral("stop"),
                             QStringLiteral("s"),
                             QStringLiteral("close_camera"),
                             QStringLiteral("表情"),
                             QStringLiteral("停止")}))
    {
        showExpressionWindowAndCloseOthers();
        return;
    }

    qWarning() << "unknown desktop control command:" << line;
}
