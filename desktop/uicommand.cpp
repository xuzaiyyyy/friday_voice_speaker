#include "uicommand.h"

#include <QByteArray>
#include <QDebug>
#include <QtGlobal>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

bool sendUiCommand(const QString &command)
{
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty())
    {
        return false;
    }

    QByteArray path = qgetenv("XIAOMAN_UI_COMMAND_FIFO");
    if (path.isEmpty())
    {
        path = "/tmp/friday_voice_speaker_ui.fifo";
    }

    const int fd = ::open(path.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        qWarning() << "ui command fifo open failed:" << path << strerror(errno);
        return false;
    }

    QByteArray payload = trimmed.toUtf8();
    payload.append('\n');
    const ssize_t written = ::write(fd, payload.constData(), payload.size());
    ::close(fd);
    if (written != payload.size())
    {
        qWarning() << "ui command fifo write failed:" << path << strerror(errno);
        return false;
    }
    return true;
}
