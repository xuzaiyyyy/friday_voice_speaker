#ifndef DESKTOPCONTROL_H
#define DESKTOPCONTROL_H

#include <QByteArray>
#include <QObject>
#include <QString>

class QSocketNotifier;

class DesktopControlPipe : public QObject
{
public:
    explicit DesktopControlPipe(QObject *parent = nullptr);
    ~DesktopControlPipe();

private:
    bool ensureFifo();
    void readAvailable();
    void handleLine(const QString &line);

    QString path_;
    int fd_ = -1;
    QSocketNotifier *notifier_ = nullptr;
    QByteArray pending_;
};

#endif // DESKTOPCONTROL_H
