#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QMouseEvent>
#include <calculate.h>
#include <QPropertyAnimation>
#include <QTimer>
#include <QDateTime>
#include <QString>
#include <QHash>
#include <QRect>
#include <photo.h>
#include <sketchpad.h>
#include <camerapage.h>
#include <serverpage.h>
#include <netcamerapage.h>
#include <musicpage.h>
#include <weatherpage.h>
#include <filemanager.h>
#include <chatclient.h>
#include <door.h>

class QResizeEvent;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();


private slots:
    void on_btn_photo_clicked();
    void on_btn_calculate_clicked();
    void on_btn_camera_clicked();

    void updateTime(); // 更新时间槽函数

    void on_btn_close_clicked();

    void on_btn_printbrush_clicked();

    void on_btn_server_clicked();

    void on_btn_netcamera_clicked();

    void on_btn_weather_clicked();

    void on_btn_music_clicked();

    void on_btn_fileview_clicked();

    void on_btn_wechat_clicked();

    void on_btn_door_clicked();

private:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void updateResponsiveLayout();


private:
    Ui::MainWindow *ui;
    QPoint diff_pos;

    QTimer *m_timer; // 定时器对象

    QPoint m_offset; //记录鼠标按下点相对于容器左上角的偏移
    bool m_isPressed; // 记录当前鼠标是否处于按下状态
    QPropertyAnimation *m_animation; // 用于处理滑动后的平滑回弹动画
    QHash<QWidget *, QRect> m_originalFrameGeometry;
};
#endif // MAINWINDOW_H
