#include "camerapage.h"
#include "ui_camerapage.h"
#include "apppaths.h"
#include "desktopidle.h"
#include <QColor>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QCameraViewfinderSettings>
#include <QFileInfo>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QBoxLayout>
#include <QResizeEvent>
#include <QTimer>
#include <QtGlobal>

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

constexpr int kDefaultRecordFps = 15;
constexpr int kMaxRecordQueueFrames = 8;

static QString gstQuotedPath(const QString &path)
{
    QString escaped = path;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    return QStringLiteral("\"%1\"").arg(escaped);
}

static QString pathWithSuffix(const QString &path, const QString &suffix)
{
    QFileInfo info(path);
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + suffix;
}

static int evenDimension(int value)
{
    return qMax(16, value & ~1);
}

static QString cameraRecordBackend()
{
    return QString::fromLocal8Bit(qgetenv("XIAOMAN_CAMERA_RECORD_BACKEND"))
        .trimmed()
        .toLower();
}

static QImage rgbMatToImage(const cv::Mat &rgb)
{
    if (rgb.empty())
    {
        return QImage();
    }
    return QImage(rgb.data, rgb.cols, rgb.rows,
                  static_cast<int>(rgb.step),
                  QImage::Format_RGB888).copy();
}

static QImage imageFromMappedVideoFrame(QVideoFrame &frame)
{
    const int width = frame.width();
    const int height = frame.height();
    const int stride = frame.bytesPerLine();
    uchar *bits = frame.bits();
    if (width <= 0 || height <= 0 || stride <= 0 || bits == nullptr)
    {
        return QImage();
    }

    const QImage::Format imageFormat =
        QVideoFrame::imageFormatFromPixelFormat(frame.pixelFormat());
    if (imageFormat != QImage::Format_Invalid)
    {
        return QImage(bits, width, height, stride, imageFormat)
            .convertToFormat(QImage::Format_RGB888)
            .copy();
    }

    cv::Mat rgb;
    switch (frame.pixelFormat())
    {
    case QVideoFrame::Format_YUYV:
    {
        cv::Mat yuyv(height, width, CV_8UC2, bits, static_cast<size_t>(stride));
        cv::cvtColor(yuyv, rgb, cv::COLOR_YUV2RGB_YUY2);
        return rgbMatToImage(rgb);
    }
    case QVideoFrame::Format_UYVY:
    {
        cv::Mat uyvy(height, width, CV_8UC2, bits, static_cast<size_t>(stride));
        cv::cvtColor(uyvy, rgb, cv::COLOR_YUV2RGB_UYVY);
        return rgbMatToImage(rgb);
    }
    case QVideoFrame::Format_NV12:
    case QVideoFrame::Format_NV21:
    {
        const int uvHeight = height / 2;
        const int totalRows = height + uvHeight;
        if (frame.mappedBytes() < stride * totalRows)
        {
            return QImage();
        }
        std::vector<uchar> packed(static_cast<size_t>(width * totalRows));
        const uchar *src = bits;
        for (int y = 0; y < height; ++y)
        {
            memcpy(packed.data() + static_cast<size_t>(y * width),
                   src + static_cast<size_t>(y * stride),
                   static_cast<size_t>(width));
        }
        const uchar *uv = src + static_cast<size_t>(stride * height);
        for (int y = 0; y < uvHeight; ++y)
        {
            memcpy(packed.data() + static_cast<size_t>((height + y) * width),
                   uv + static_cast<size_t>(y * stride),
                   static_cast<size_t>(width));
        }
        cv::Mat yuv(totalRows, width, CV_8UC1, packed.data());
        cv::cvtColor(yuv, rgb, frame.pixelFormat() == QVideoFrame::Format_NV12
                                   ? cv::COLOR_YUV2RGB_NV12
                                   : cv::COLOR_YUV2RGB_NV21);
        return rgbMatToImage(rgb);
    }
    case QVideoFrame::Format_YUV420P:
    case QVideoFrame::Format_YV12:
    {
        const int packedBytes = width * height * 3 / 2;
        if (frame.mappedBytes() < packedBytes)
        {
            return QImage();
        }
        cv::Mat yuv(height * 3 / 2, width, CV_8UC1, bits);
        cv::cvtColor(yuv, rgb, frame.pixelFormat() == QVideoFrame::Format_YUV420P
                                   ? cv::COLOR_YUV2RGB_I420
                                   : cv::COLOR_YUV2RGB_YV12);
        return rgbMatToImage(rgb);
    }
    case QVideoFrame::Format_BGR24:
    {
        cv::Mat bgr(height, width, CV_8UC3, bits, static_cast<size_t>(stride));
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        return rgbMatToImage(rgb);
    }
    case QVideoFrame::Format_BGRA32:
    case QVideoFrame::Format_BGR32:
    {
        cv::Mat bgra(height, width, CV_8UC4, bits, static_cast<size_t>(stride));
        cv::cvtColor(bgra, rgb, cv::COLOR_BGRA2RGB);
        return rgbMatToImage(rgb);
    }
    case QVideoFrame::Format_Y8:
    {
        cv::Mat gray(height, width, CV_8UC1, bits, static_cast<size_t>(stride));
        cv::cvtColor(gray, rgb, cv::COLOR_GRAY2RGB);
        return rgbMatToImage(rgb);
    }
    case QVideoFrame::Format_Jpeg:
    {
        std::vector<uchar> encoded(bits, bits + frame.mappedBytes());
        cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
        if (decoded.empty())
        {
            return QImage();
        }
        cv::cvtColor(decoded, rgb, cv::COLOR_BGR2RGB);
        return rgbMatToImage(rgb);
    }
    default:
        return QImage();
    }
}

class PreviewFrameRecorder
{
public:
    ~PreviewFrameRecorder()
    {
        stop();
    }

    bool start(const QString &path, int fps, const QSize &size)
    {
        stop();
        if (path.isEmpty())
        {
            return false;
        }

        const int width = evenDimension(size.width());
        const int height = evenDimension(size.height());
        frameSize_ = cv::Size(width, height);
        fps_ = qBound(5, fps, 30);

        QString actualPath = pathWithSuffix(path, QStringLiteral(".mp4"));
        QString backend = cameraRecordBackend();
        if (backend.isEmpty())
        {
            backend = QStringLiteral("auto");
        }

        bool opened = false;
        if (backend == QStringLiteral("auto") || backend == QStringLiteral("mpp") || backend == QStringLiteral("h264"))
        {
            opened = openGStreamerH264Writer(actualPath, QStringLiteral("mpp"));
            if (!opened && backend != QStringLiteral("mpp"))
            {
                opened = openGStreamerH264Writer(actualPath, QStringLiteral("x264"));
            }
        }

        if (!opened && backend != QStringLiteral("mpp") && backend != QStringLiteral("h264"))
        {
            actualPath = pathWithSuffix(path, QStringLiteral(".avi"));
            opened = openMjpgWriter(actualPath);
        }

        if (!opened)
        {
            qWarning() << "opencv recorder open failed:" << path;
            writer_.release();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            path_ = actualPath;
            stopRequested_ = false;
            running_ = true;
            queue_.clear();
        }

        worker_ = std::thread([this]() {
            run();
        });
        qDebug() << "opencv recorder started:" << actualPath
                 << "backend=" << activeBackend_
                 << "fps=" << fps_
                 << "size=" << width << "x" << height;
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && !worker_.joinable())
            {
                if (writer_.isOpened())
                {
                    writer_.release();
                }
                return;
            }
            stopRequested_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable())
        {
            worker_.join();
        }
        if (writer_.isOpened())
        {
            writer_.release();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            stopRequested_ = false;
            queue_.clear();
        }
        if (!path_.isEmpty())
        {
            qDebug() << "opencv recorder stopped:" << path_;
        }
    }

    bool isRunning() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ && !stopRequested_;
    }

    void pushFrame(const QImage &image)
    {
        if (image.isNull())
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || stopRequested_)
            {
                return;
            }
            if (queue_.size() >= kMaxRecordQueueFrames)
            {
                queue_.pop_front();
            }
            queue_.push_back(image.copy());
        }
        condition_.notify_one();
    }

private:
    bool openGStreamerH264Writer(const QString &path, const QString &encoder)
    {
        QString encoderElement;
        if (encoder == QStringLiteral("mpp"))
        {
            encoderElement = QStringLiteral("mpph264enc");
        }
        else if (encoder == QStringLiteral("x264"))
        {
            encoderElement = QStringLiteral("x264enc tune=zerolatency speed-preset=ultrafast key-int-max=%1 bitrate=2500")
                                 .arg(qMax(5, fps_));
        }
        else
        {
            return false;
        }

        const QString pipeline = QStringLiteral(
            "appsrc is-live=true block=true format=time do-timestamp=true ! "
            "video/x-raw,format=BGR,width=%1,height=%2,framerate=%3/1 ! "
            "queue max-size-buffers=8 leaky=downstream ! "
            "videoconvert ! video/x-raw,format=NV12 ! "
            "%4 ! h264parse ! mp4mux ! filesink location=%5 sync=false")
                                     .arg(frameSize_.width)
                                     .arg(frameSize_.height)
                                     .arg(fps_)
                                     .arg(encoderElement)
                                     .arg(gstQuotedPath(path));

        if (writer_.open(pipeline.toStdString(), cv::CAP_GSTREAMER, 0, fps_, frameSize_, true))
        {
            activeBackend_ = encoder == QStringLiteral("mpp")
                                 ? QStringLiteral("gstreamer-mpp-h264")
                                 : QStringLiteral("gstreamer-x264");
            return true;
        }

        writer_.release();
        qWarning() << "gstreamer h264 recorder open failed:" << encoder << path;
        return false;
    }

    bool openMjpgWriter(const QString &path)
    {
        const int fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
        if (!writer_.open(path.toStdString(), fourcc, fps_, frameSize_, true))
        {
            writer_.release();
            return false;
        }
        activeBackend_ = QStringLiteral("opencv-mjpg");
        return true;
    }

    static cv::Mat toBgrMat(const QImage &image, const cv::Size &targetSize)
    {
        QImage rgb = image.convertToFormat(QImage::Format_RGB888);
        cv::Mat rgbMat(rgb.height(), rgb.width(), CV_8UC3,
                       const_cast<uchar *>(rgb.constBits()),
                       static_cast<size_t>(rgb.bytesPerLine()));
        cv::Mat bgr;
        cv::cvtColor(rgbMat, bgr, cv::COLOR_RGB2BGR);
        if (bgr.size() != targetSize)
        {
            cv::resize(bgr, bgr, targetSize, 0, 0, cv::INTER_LINEAR);
        }
        return bgr;
    }

    void run()
    {
        for (;;)
        {
            QImage image;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() {
                    return stopRequested_ || !queue_.empty();
                });
                if (stopRequested_ && queue_.empty())
                {
                    break;
                }
                image = queue_.front();
                queue_.pop_front();
            }

            if (!image.isNull() && writer_.isOpened())
            {
                writer_.write(toBgrMat(image, frameSize_));
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QImage> queue_;
    std::thread worker_;
    cv::VideoWriter writer_;
    cv::Size frameSize_{640, 480};
    QString path_;
    QString activeBackend_;
    int fps_ = kDefaultRecordFps;
    bool running_ = false;
    bool stopRequested_ = false;
};

int cameraRecordFps()
{
    bool ok = false;
    const int fps = qEnvironmentVariableIntValue("XIAOMAN_CAMERA_RECORD_FPS", &ok);
    return qBound(5, ok ? fps : kDefaultRecordFps, 30);
}

CameraPage::CameraPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::CameraPage),
    camera(nullptr),
    viewfinder(nullptr),
    imageCapture(nullptr),
    videoProbe(nullptr),
    previewFreezeOverlay(nullptr),
    frameRecorder(new PreviewFrameRecorder()),
    recordFallbackTimer(new QTimer(this)),
    previewFreezeGeneration(0),
    recordFps(cameraRecordFps()),
    recordFrameIntervalMs(qMax(1, 1000 / recordFps)),
    lastProbeFrameMs(0),
    lastRecordedFrameMs(0),
    photoPath(AppPaths::ensureDesktopDir("photos")),
    videoPath(AppPaths::ensureDesktopDir("videos"))
{
    ui->setupUi(this);

    this->setWindowFlags(Qt::FramelessWindowHint);
    //this->setAttribute(Qt::WA_TranslucentBackground);

    // 1. 确保文件夹存在
    QDir dir;
    if (!dir.exists(photoPath)) dir.mkpath(photoPath);
    if (!dir.exists(videoPath)) dir.mkpath(videoPath);

    currentMode = Mode_Photo;
    isRecording = false;

    setupUI();
    initCamera();
    setMode(Mode_Photo);
}

CameraPage::~CameraPage()
{
    stopVideoRecording();
    if (camera) {
        camera->stop();
        delete camera;
    }
    delete frameRecorder;
    delete ui;
}


void CameraPage::setupUI()
{
    this->setStyleSheet("background-color: black;");
    ui->btn_shutter->setText("");
    ui->btn_shutter->setStyleSheet(
        "QPushButton { background-color: white; border-radius: 40px; border: 4px solid #D3D3D3; }"
        "QPushButton:pressed { background-color: #E0E0E0; }"
    );
    // iOS 风格样式
    QString styleSelected = "color: #FFD700; font-weight: bold; font-size: 16px;"
                            "background-color: rgba(255, 255, 255, 30);" // 玻璃底纹
                            "border-radius: 12px; padding: 4px 10px;";

    QString styleUnselected = "color: white; font-size: 14px; background: transparent;";

    ui->lbl_photo->setStyleSheet(styleSelected);   // 照片设为选中状态
    ui->lbl_video->setStyleSheet(styleUnselected); // 视频设为普通状态

    ui->lbl_photo->setAlignment(Qt::AlignCenter);
    ui->lbl_video->setAlignment(Qt::AlignCenter);
    ui->lbl_photo->installEventFilter(this);
    ui->lbl_video->installEventFilter(this);

    // 初始化相册按钮样式
    ui->btn_gallery->setStyleSheet(
        "QPushButton { "
        "   border-radius: 27px; "
        "   background-color: #222; " // 没照片时的底色
        "   border: 2px solid rgba(255, 255, 255, 100); "
        "}"
    );

    // 启动时立刻去文件夹找最后一张照片显示
    updateGalleryIcon();

    previewFreezeOverlay = new QLabel(ui->widget_viewfinder);
    previewFreezeOverlay->setAlignment(Qt::AlignCenter);
    previewFreezeOverlay->setStyleSheet(QStringLiteral("background-color: black;"));
    previewFreezeOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    previewFreezeOverlay->hide();

    recordFallbackTimer->setTimerType(Qt::PreciseTimer);
    recordFallbackTimer->setInterval(recordFrameIntervalMs);
    connect(recordFallbackTimer, &QTimer::timeout,
            this, &CameraPage::captureFallbackRecordFrame);

    updateResponsiveLayout();
}

void CameraPage::initCamera()
{
    // 1. 获取系统所有可用摄像头列表
    QList<QCameraInfo> cameras = QCameraInfo::availableCameras();

    if (cameras.isEmpty()) {
        qDebug() << "错误：Qt 找不到任何摄像头！请检查驱动或权限。";
        return;
    }

    // 2. 打印调试信息
    foreach (const QCameraInfo &cameraInfo, cameras) {
        qDebug() << "发现摄像头:" << cameraInfo.deviceName() << cameraInfo.description();
    }

    // 3. 显式选择第一个可用摄像头
    // 提示：如果 /dev/video0 是黑屏，可以尝试改为 cameras[1]（如果有的话）
    QCameraInfo targetCamera = cameras[0];

    // 也可以通过描述来筛选 USB 摄像头
    // for (const QCameraInfo &info : cameras) {
    //    if (info.description().contains("USB", Qt::CaseInsensitive)) {
    //        targetCamera = info;
    //        break;
    //    }
    // }

    qDebug() << "正在尝试连接:" << targetCamera.deviceName();

    // 4. 使用指定的摄像头创建对象
    camera = new QCamera(targetCamera, this);

    // 5. 设置取景器
    viewfinder = new QCameraViewfinder(this);
    QVBoxLayout *layout = new QVBoxLayout(ui->widget_viewfinder);
    layout->setMargin(0);
    layout->addWidget(viewfinder);

    camera->setViewfinder(viewfinder);

    // 应用配置
    applyCameraSettings();

    imageCapture = new QCameraImageCapture(camera);
    connect(imageCapture, &QCameraImageCapture::imageSaved, this, &CameraPage::onImageSaved);

    videoProbe = new QVideoProbe(this);
    connect(videoProbe, &QVideoProbe::videoFrameProbed,
            this, &CameraPage::onVideoFrameProbed);
    if (!videoProbe->setSource(camera)) {
        qDebug() << "提示：当前 Qt 摄像头后端不支持 QVideoProbe，录像会退回到取景器截图。";
    }

    camera->setCaptureMode(QCamera::CaptureStillImage);
    qDebug() << "相机默认照片模式，录像使用后台预览帧编码";
}

// 事件过滤器
bool CameraPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin) {
        if (watched == ui->lbl_photo) {
            setMode(Mode_Photo);
            return true;
        } else if (watched == ui->lbl_video) {
            setMode(Mode_Video);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// 模式切换
void CameraPage::setMode(CameraMode mode)
{
    if (currentMode == mode) return;

    if (isRecording) {
        stopVideoRecording();
    }

    currentMode = mode;
    isRecording = false;

    // UI 样式切换
    QString styleSelected = "color: #FFD700; font-weight: bold; font-size: 16px;"
                            "background-color: rgba(255, 255, 255, 30); border-radius: 12px; padding: 4px 10px;";
    QString styleUnselected = "color: rgba(255, 255, 255, 100); background: transparent; font-size: 14px;";

    if (mode == Mode_Photo) {
        ui->lbl_photo->setStyleSheet(styleSelected);
        ui->lbl_video->setStyleSheet(styleUnselected);
    } else {
        ui->lbl_video->setStyleSheet(styleSelected);
        ui->lbl_photo->setStyleSheet(styleUnselected);
    }

    setShutterRecordingStyle(false);
    updateResponsiveLayout();
    if (camera != nullptr && camera->state() != QCamera::ActiveState) {
        applyCameraSettings();
        camera->start();
    }
}

// 【核心】快门点击逻辑
void CameraPage::on_btn_shutter_clicked()
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");

    if (currentMode == Mode_Photo) {
        QString fileName = QString("%1/IMG_%2.jpg").arg(photoPath).arg(timestamp);
        capturePhoto(fileName);
    }
    else {
        if (!isRecording) {
            QString fileName = QString("%1/VID_%2.mp4").arg(videoPath).arg(timestamp);
            startVideoRecording(fileName);
        } else {
            stopVideoRecording();
        }
    }
}

// 拍照成功回调
void CameraPage::onImageSaved(int id, const QString &fileName)
{
    Q_UNUSED(id);
    qDebug() << "图片保存成功:" << fileName;
    updateGalleryIcon(fileName);
}

void CameraPage::onVideoFrameProbed(const QVideoFrame &frame)
{
    QVideoFrame copy(frame);
    if (!copy.map(QAbstractVideoBuffer::ReadOnly)) {
        return;
    }

    QImage image = imageFromMappedVideoFrame(copy);
    static int lastLoggedPixelFormat = -9999;
    if (copy.pixelFormat() != lastLoggedPixelFormat) {
        lastLoggedPixelFormat = copy.pixelFormat();
        qDebug() << "camera probe frame format=" << copy.pixelFormat()
                 << "size=" << copy.width() << "x" << copy.height()
                 << "stride=" << copy.bytesPerLine()
                 << "decoded=" << !image.isNull();
    }

    if (!image.isNull()) {
        lastPreviewImage = image.copy();
        lastProbeFrameMs = QDateTime::currentMSecsSinceEpoch();
        submitRecordingFrame(lastPreviewImage);
    }

    copy.unmap();
}

void CameraPage::capturePhoto(const QString &fileName)
{
    if (camera == nullptr || imageCapture == nullptr) {
        return;
    }

    auto doCapture = [=]() {
        if (camera == nullptr || imageCapture == nullptr || currentMode != Mode_Photo) {
            hidePreviewFreeze();
            return;
        }
        if (camera->state() != QCamera::ActiveState) {
            camera->start();
        }
        camera->searchAndLock();
        imageCapture->capture(fileName);
        camera->unlock();
        QTimer::singleShot(220, this, &CameraPage::hidePreviewFreeze);
        qDebug() << "正在拍照..." << fileName;
    };

    if (!(camera->captureMode() & QCamera::CaptureStillImage)) {
        setCameraCaptureMode(QCamera::CaptureStillImage, true);
        QTimer::singleShot(320, this, doCapture);
        return;
    }

    doCapture();
}

void CameraPage::startVideoRecording(const QString &fileName)
{
    if (camera == nullptr || frameRecorder == nullptr || fileName.isEmpty()) {
        return;
    }

    if (isRecording) {
        stopVideoRecording();
    }

    if (camera->state() != QCamera::ActiveState) {
        applyCameraSettings();
        camera->start();
    }

    const QSize recordSize = lastPreviewImage.isNull()
                                 ? QSize(640, 480)
                                 : lastPreviewImage.size();
    if (!frameRecorder->start(fileName, recordFps, recordSize)) {
        isRecording = false;
        setShutterRecordingStyle(false);
        qDebug() << "后台录像启动失败:" << fileName;
        return;
    }

    isRecording = true;
    lastRecordedFrameMs = 0;
    lastProbeFrameMs = 0;
    setShutterRecordingStyle(true);
    if (recordFallbackTimer != nullptr) {
        recordFallbackTimer->start(recordFrameIntervalMs);
    }
    if (!lastPreviewImage.isNull()) {
        submitRecordingFrame(lastPreviewImage);
    } else {
        captureFallbackRecordFrame();
    }
    qDebug() << "后台录像启动:" << fileName;
}

void CameraPage::stopVideoRecording()
{
    if (!isRecording && (frameRecorder == nullptr || !frameRecorder->isRunning())) {
        return;
    }

    isRecording = false;
    if (recordFallbackTimer != nullptr) {
        recordFallbackTimer->stop();
    }
    if (frameRecorder != nullptr) {
        frameRecorder->stop();
    }
    hidePreviewFreeze();
    updateGalleryIcon();
    setShutterRecordingStyle(false);
    qDebug() << "后台录像停止";
}

void CameraPage::captureFallbackRecordFrame()
{
    if (!isRecording || frameRecorder == nullptr || !frameRecorder->isRunning()) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastRecordedFrameMs > 0 && now - lastRecordedFrameMs < recordFrameIntervalMs) {
        return;
    }

    if (!lastPreviewImage.isNull()) {
        submitRecordingFrame(lastPreviewImage);
        return;
    }

    if (viewfinder == nullptr) {
        return;
    }

    const QPixmap pixmap = viewfinder->grab();
    if (!pixmap.isNull()) {
        submitRecordingFrame(pixmap.toImage());
    }
}

void CameraPage::submitRecordingFrame(const QImage &image)
{
    if (!isRecording || frameRecorder == nullptr || !frameRecorder->isRunning() || image.isNull()) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastRecordedFrameMs > 0 && now - lastRecordedFrameMs < recordFrameIntervalMs) {
        return;
    }

    lastRecordedFrameMs = now;
    frameRecorder->pushFrame(image);
}

bool CameraPage::setCameraCaptureMode(QCamera::CaptureModes mode, bool maskPreview)
{
    if (camera == nullptr) {
        return false;
    }
    const QCamera::CaptureModes combinedMode =
        QCamera::CaptureModes(QCamera::CaptureStillImage) | QCamera::CaptureVideo;
    if ((mode & combinedMode) && camera->isCaptureModeSupported(combinedMode)) {
        mode = combinedMode;
    }
    const QCamera::CaptureModes currentMode = camera->captureMode();
    if (currentMode == mode || ((currentMode & mode) == mode)) {
        return false;
    }
    if (maskPreview) {
        showPreviewFreeze(900);
    }
    camera->setCaptureMode(mode);
    applyCameraSettings();
    return true;
}

void CameraPage::showPreviewFreeze(int keepMs)
{
    if (previewFreezeOverlay == nullptr || ui->widget_viewfinder == nullptr) {
        return;
    }

    ++previewFreezeGeneration;
    previewFreezeOverlay->setGeometry(ui->widget_viewfinder->rect());
    QPixmap pixmap;
    if (!lastPreviewImage.isNull()) {
        pixmap = QPixmap::fromImage(lastPreviewImage);
    }
    if (pixmap.isNull()) {
        pixmap = ui->widget_viewfinder->grab();
    }
    if (!pixmap.isNull()) {
        previewFreezeOverlay->setPixmap(pixmap.scaled(previewFreezeOverlay->size(),
                                                      Qt::KeepAspectRatioByExpanding,
                                                      Qt::SmoothTransformation));
    } else {
        return;
    }
    previewFreezeOverlay->show();
    previewFreezeOverlay->raise();

    if (keepMs > 0) {
        const int generation = previewFreezeGeneration;
        QTimer::singleShot(keepMs, this, [this, generation]() {
            if (generation == previewFreezeGeneration) {
                hidePreviewFreeze();
            }
        });
    }
}

void CameraPage::hidePreviewFreeze()
{
    if (previewFreezeOverlay != nullptr) {
        ++previewFreezeGeneration;
        previewFreezeOverlay->hide();
        previewFreezeOverlay->clear();
    }
}

void CameraPage::setShutterRecordingStyle(bool recording)
{
    const int shutterSize = qMax(1, ui->btn_shutter->width());
    const int shutterRadius = shutterSize / 2;
    if (recording) {
        ui->btn_shutter->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #FF3B30; border-radius: %1px; border: 4px solid white; }"
            "QPushButton:pressed { background-color: #D82920; }")
                                           .arg(shutterRadius));
        return;
    }

    ui->btn_shutter->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: white; border-radius: %1px; border: 4px solid #D3D3D3; }"
        "QPushButton:pressed { background-color: #E0E0E0; }")
                                       .arg(shutterRadius));
}

void CameraPage::on_photo_back_clicked()
{
    auto *w = new MainWindow();
    replaceDesktopTopLevel(this, w);
}

void CameraPage::applyCameraSettings()
{
    if (camera == nullptr) {
        return;
    }

    QCameraViewfinderSettings settings;
    settings.setResolution(640, 480);
    settings.setMinimumFrameRate(30.0);
    settings.setMaximumFrameRate(30.0);
    //settings.setPixelFormat(QVideoFrame::Format_YUYV);

    // 2. 🛑 【核心修改】强制使用 MJPEG 格式！
    // 这样就和你的 gst-launch 命令一样了
    // 之前的 YUYV 会导致需要重编码，现在我们直接用原生流
    //settings.setPixelFormat(QVideoFrame::Format_Jpeg);

    camera->setViewfinderSettings(settings);
}

void CameraPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    setMode(Mode_Photo);

    if (camera != nullptr && camera->state() != QCamera::ActiveState) {
        applyCameraSettings();
        camera->start();
        qDebug() << "相机页面显示：摄像头已开启";
    }
}

void CameraPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (isRecording) {
        stopVideoRecording();
    }
    if (camera != nullptr && camera->state() == QCamera::ActiveState) {
        camera->stop();
        qDebug() << "相机页面隐藏：摄像头已关闭";
    }
}

void CameraPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void CameraPage::updateResponsiveLayout()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
    {
        return;
    }

    const int shortSide = qMin(w, h);
    const int controlHeight = qBound(96, h / 4, 132);
    const int viewHeight = qMax(1, h - controlHeight);
    const int margin = qBound(10, shortSide / 38, 16);
    const int shutterSize = qBound(58, shortSide / 6, 76);
    const int sideButton = qBound(44, shortSide / 9, 58);
    const int labelW = qBound(56, w / 12, 78);
    const int labelH = qBound(34, controlHeight / 3, 44);
    const int centerY = (controlHeight - shutterSize) / 2;

    ui->widget_viewfinder->setGeometry(0, 0, w, viewHeight);
    if (previewFreezeOverlay != nullptr) {
        previewFreezeOverlay->setGeometry(ui->widget_viewfinder->rect());
    }
    ui->control_bar->setGeometry(0, viewHeight, w, controlHeight);
    ui->btn_shutter->setFixedSize(shutterSize, shutterSize);
    ui->btn_shutter->setGeometry((w - shutterSize) / 2, centerY, shutterSize, shutterSize);
    ui->btn_gallery->setFixedSize(sideButton, sideButton);
    ui->btn_gallery->setGeometry(margin * 2, (controlHeight - sideButton) / 2, sideButton, sideButton);
    ui->photo_back->setGeometry(w - margin * 2 - sideButton,
                                (controlHeight - sideButton) / 2,
                                sideButton,
                                sideButton);

    const int labelY = qMax(8, (controlHeight - labelH) / 2);
    ui->lbl_video->setGeometry(qMax(margin, w / 2 - shutterSize / 2 - labelW * 2 - margin),
                               labelY,
                               labelW,
                               labelH);
    ui->lbl_photo->setGeometry(qMin(w - margin - labelW, w / 2 + shutterSize / 2 + margin),
                               labelY,
                               labelW,
                               labelH);

    setShutterRecordingStyle(isRecording);

    const int galleryRadius = sideButton / 2;
    ui->btn_gallery->setStyleSheet(QStringLiteral(
        "QPushButton { border-radius: %1px; background-color: #222; border: 2px solid rgba(255, 255, 255, 100); }")
                                       .arg(galleryRadius));
    ui->btn_gallery->setIconSize(QSize(sideButton, sideButton));
    ui->photo_back->raise();
    ui->btn_gallery->raise();
    ui->btn_shutter->raise();
}

// 裁剪成圆形
QPixmap clipToCircle(const QPixmap& src, int totalDiameter, qreal borderWidth) {
    if (src.isNull()) return QPixmap();

    QRectF outerRect(0, 0, totalDiameter, totalDiameter);
    QSize finalSize(totalDiameter, totalDiameter);

    QPixmap dest(finalSize);
    dest.fill(Qt::transparent);

    QPainter painter(&dest);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::HighQualityAntialiasing, true);

    // 画背景圆
    QColor borderColor(255, 255, 255, 180);
    painter.setPen(Qt::NoPen);
    painter.setBrush(borderColor);
    painter.drawEllipse(outerRect);

    // 图片区域
    QRectF innerRect = outerRect.adjusted(borderWidth, borderWidth, -borderWidth, -borderWidth);
    QPainterPath clipPath;
    clipPath.addEllipse(innerRect);
    painter.setClipPath(clipPath);

    // 绘制图片
    QSize innerSize = innerRect.size().toSize();
    QPixmap scaled = src.scaled(innerSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    qreal x = innerRect.x() + (innerRect.width() - scaled.width()) / 2.0;
    qreal y = innerRect.y() + (innerRect.height() - scaled.height()) / 2.0;

    painter.drawPixmap(QPointF(x, y), scaled);

    return dest;
}

void CameraPage::updateGalleryIcon(const QString &targetFile)
{
    QString filePath = targetFile;

    if (filePath.isEmpty()) {
        QDir dir(photoPath);
        QStringList filters;
        filters << "*.jpg" << "*.png" << "*.jpeg";
        dir.setNameFilters(filters);
        dir.setFilter(QDir::Files | QDir::NoDotAndDotDot);
        dir.setSorting(QDir::Time | QDir::Reversed);

        QFileInfoList list = dir.entryInfoList();
        if (!list.isEmpty()) {
            filePath = list.first().absoluteFilePath();
        } else {
            return;
        }
    }

    QPixmap pix(filePath);
    if (pix.isNull()) return;

    const int buttonSize = qMax(40, qMin(ui->btn_gallery->width(), ui->btn_gallery->height()));
    QPixmap circularPix = clipToCircle(pix, buttonSize, 3.0);

    ui->btn_gallery->setIcon(QIcon(circularPix));
    ui->btn_gallery->setIconSize(QSize(buttonSize, buttonSize));

    qDebug() << "缩略图更新完毕:" << filePath;
}

void CameraPage::on_btn_gallery_clicked()
{
    auto *p = new photo();
    replaceDesktopTopLevel(this, p);
}
