#ifndef LIVECAMERAWINDOW_H
#define LIVECAMERAWINDOW_H

#include <QDateTime>
#include <QPixmap>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QKeyEvent;
class QResizeEvent;
class QTimer;

class LiveCameraWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LiveCameraWindow(const QString &title, const QString &command, QWidget *parent = nullptr);
    ~LiveCameraWindow();
    void setDisplayTitle(const QString &title);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void refreshPreview();
    void stopAndReturn();

private:
    void renderPixmap();

    QString title_;
    QString command_;
    QString previewPath_;
    QLabel *imageLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QPushButton *stopButton_ = nullptr;
    QTimer *timer_ = nullptr;
    QPixmap currentPixmap_;
    QDateTime lastModified_;
    bool stopSent_ = false;
};

#endif // LIVECAMERAWINDOW_H
