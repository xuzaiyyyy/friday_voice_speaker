#ifndef CAMERAPAGE_H
#define CAMERAPAGE_H

#include <QWidget>
#include <QCamera>
#include <QCameraViewfinder>
#include <QCameraImageCapture>
#include <QVBoxLayout>
#include <QDebug>
#include <mainwindow.h>
#include <QMouseEvent> // 必须引入鼠标事件头文件
#include <QDir>           // 用于操作文件夹
#include <QCameraInfo>
#include <QThread>
#include <QDir>
#include <QImage>
#include <QPixmap>
#include <QAbstractVideoBuffer>
#include <QVideoFrame>
#include <QVideoProbe>
#include <photo.h>

class QLabel;
class QResizeEvent;
class QTimer;
class PreviewFrameRecorder;

namespace Ui {
class CameraPage;
}

class CameraPage : public QWidget
{
    Q_OBJECT

public:
    explicit CameraPage(QWidget *parent = nullptr);
    ~CameraPage();

private slots:
    void on_btn_shutter_clicked();

    // 【新增】用来接收拍照保存成功的信号
    void onImageSaved(int id, const QString &fileName);

    void onVideoFrameProbed(const QVideoFrame &frame);


    void on_photo_back_clicked();

    void on_btn_gallery_clicked();

protected:
    // 【新增】重写事件过滤器，用来“监听”文字标签的点击
    bool eventFilter(QObject *watched, QEvent *event) override;

    // 窗口显示和隐藏的事件，用于自动开关摄像头
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;



private:
    Ui::CameraPage *ui;

    QCamera *camera;
    QCameraViewfinder *viewfinder;
    QCameraImageCapture *imageCapture;
    QVideoProbe *videoProbe;
    QLabel *previewFreezeOverlay;
    PreviewFrameRecorder *frameRecorder;
    QTimer *recordFallbackTimer;
    QImage lastPreviewImage;
    int previewFreezeGeneration;
    int recordFps;
    int recordFrameIntervalMs;
    qint64 lastProbeFrameMs;
    qint64 lastRecordedFrameMs;

    void updateGalleryIcon(const QString &targetFile = "");

    //封装一个函数，用来应对黄金配置
    void applyCameraSettings();

    enum CameraMode {
        Mode_Photo,
        Mode_Video
    };
    CameraMode currentMode;
    bool isRecording;

    QString photoPath;
    QString videoPath;

    void setupUI();
    void initCamera();
    void setMode(CameraMode mode);
    void updateResponsiveLayout();
    void capturePhoto(const QString &fileName);
    void startVideoRecording(const QString &fileName);
    void stopVideoRecording();
    void captureFallbackRecordFrame();
    void submitRecordingFrame(const QImage &image);
    bool setCameraCaptureMode(QCamera::CaptureModes mode, bool maskPreview);
    void showPreviewFreeze(int keepMs);
    void hidePreviewFreeze();
    void setShutterRecordingStyle(bool recording);
};

#endif // CAMERAPAGE_H
