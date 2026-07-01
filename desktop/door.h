#ifndef DOOR_H
#define DOOR_H

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QPushButton>
#include <QImage>
#include <facethread.h>
#include <QCloseEvent>
#include <QDebug>
#include <QHBoxLayout>
#include <mainwindow.h>

class QResizeEvent;

namespace Ui {
class Door;
}

class Door : public QWidget
{
    Q_OBJECT

public:
    explicit Door(QWidget *parent = nullptr);
    ~Door();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    // === 接收 AI 线程信号 ===
    void updateImage(QImage image);   // 更新视频画面
    void updateLog(QString msg);      // 显示日志（可选）

    void on_pushButton_clicked();

    void on_pushButton_2_clicked();

    void on_pushButton_3_clicked();

    void on_pushButton_4_clicked();

    void on_photo_back_clicked();
    void confirmInlineInput();
    void cancelInlineInput();

private:
    enum class InputStep {
        None,
        RegisterPassword,
        RegisterName,
        DeletePassword,
        DeleteName
    };

    void setupInlineInputPanel();
    void updateOverlayGeometry();
    void showInlineInput(InputStep step);
    void hideInlineInput();
    void setStatusText(const QString &text);
    QString defaultFaceName() const;

    Ui::Door *ui;
    FaceThread *m_faceThread;  // AI 工作线程
    QLabel *m_displayLabel;    // 用于在界面右侧显示视频
    QLabel *m_statusLabel;
    QFrame *m_inputPanel;
    QLabel *m_inputTitle;
    QLabel *m_inputHint;
    QLineEdit *m_inputEdit;
    QPushButton *m_inputConfirm;
    QPushButton *m_inputCancel;
    QWidget *m_digitPad;
    InputStep m_inputStep;
};

#endif // DOOR_H
