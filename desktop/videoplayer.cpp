#include "videoplayer.h"
#include "ui_videoplayer.h"
#include "apppaths.h"

#include <QDebug>
#include <QEvent>
#include <QFileInfo>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QStyle>
#include <QtGlobal>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <utility>

namespace
{
constexpr int kFallbackPreviewFps = 24;
constexpr int kMinOriginalFps = 5;
constexpr int kMaxOriginalFps = 60;
constexpr int kMinFrameDelayMs = 10;
constexpr int kMaxFrameDelayMs = 250;

QString gstQuotedPath(const QString &path)
{
    QString escaped = path;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    return QStringLiteral("\"%1\"").arg(escaped);
}

bool isH264Mp4Candidate(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("mp4") || suffix == QStringLiteral("mov") || suffix == QStringLiteral("m4v");
}

bool preferMppDecoder()
{
    const QString backend = QString::fromLocal8Bit(qgetenv("XIAOMAN_VIDEO_DECODE_BACKEND"))
                                .trimmed()
                                .toLower();
    return backend != QStringLiteral("opencv") && backend != QStringLiteral("soft");
}

QString mppDecodePipeline(const QString &path)
{
    return QStringLiteral(
        "filesrc location=%1 ! qtdemux ! h264parse ! "
        "mppvideodec ! videoconvert ! video/x-raw,format=BGR ! "
        "appsink sync=false drop=false max-buffers=4")
        .arg(gstQuotedPath(path));
}

struct VideoMetadata
{
    double fps = 0.0;
    int frameCount = 0;
};

VideoMetadata readVideoMetadata(const QString &path)
{
    VideoMetadata metadata;
    cv::VideoCapture meta(path.toStdString());
    if (!meta.isOpened())
    {
        return metadata;
    }
    metadata.fps = meta.get(cv::CAP_PROP_FPS);
    const double count = meta.get(cv::CAP_PROP_FRAME_COUNT);
    metadata.frameCount = count > 0.0 ? static_cast<int>(count + 0.5) : 0;
    return metadata;
}

int playbackFpsFromMetadata(double metadataFps)
{
    bool ok = false;
    int fps = qEnvironmentVariableIntValue("XIAOMAN_VIDEO_PREVIEW_FPS", &ok);
    if (ok)
    {
        return qBound(kMinOriginalFps, fps, kMaxOriginalFps);
    }

    if (metadataFps >= kMinOriginalFps && metadataFps <= kMaxOriginalFps)
    {
        return qBound(kMinOriginalFps, static_cast<int>(metadataFps + 0.5), kMaxOriginalFps);
    }

    return kFallbackPreviewFps;
}

int delayFromTimestamps(double previousMs, double currentMs, int fallbackMs)
{
    if (previousMs >= 0.0 && currentMs > previousMs)
    {
        return qBound(kMinFrameDelayMs, static_cast<int>(currentMs - previousMs + 0.5), kMaxFrameDelayMs);
    }
    return fallbackMs;
}

QVector<int> buildFrameTimelineMs(const QString &path, int fallbackFrameIntervalMs)
{
    cv::VideoCapture probe(path.toStdString());
    if (!probe.isOpened())
    {
        return {};
    }

    QVector<int> rawTimes;
    cv::Mat frame;
    while (probe.read(frame) && !frame.empty())
    {
        const double posMs = probe.get(cv::CAP_PROP_POS_MSEC);
        rawTimes.push_back(posMs >= 0.0 ? static_cast<int>(posMs + 0.5) : 0);
    }

    if (rawTimes.isEmpty())
    {
        return rawTimes;
    }

    bool monotonicTimeline = rawTimes.size() > 1;
    for (int i = 1; i < rawTimes.size(); ++i)
    {
        if (rawTimes[i] <= rawTimes[i - 1])
        {
            monotonicTimeline = false;
            break;
        }
    }

    if (monotonicTimeline && rawTimes.back() > 0)
    {
        const int first = rawTimes.front();
        for (int &value : rawTimes)
        {
            value = qMax(0, value - first);
        }
        return rawTimes;
    }

    QVector<int> syntheticTimes;
    syntheticTimes.reserve(rawTimes.size());
    for (int i = 0; i < rawTimes.size(); ++i)
    {
        syntheticTimes.push_back(i * fallbackFrameIntervalMs);
    }
    return syntheticTimes;
}
} // namespace

VideoPlayer::VideoPlayer(QString videoPath, QWidget *parent)
    : QWidget(parent),
      ui(new Ui::VideoPlayer),
      videoPath(std::move(videoPath))
{
    ui->setupUi(this);
    setWindowFlags(Qt::FramelessWindowHint);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setStyleSheet(QStringLiteral("background: #000000;"));

    QObject::disconnect(ui->sketchpad_back, nullptr, this, nullptr);
    QObject::disconnect(ui->btn_play, nullptr, this, nullptr);
    QObject::disconnect(ui->slider_seek, nullptr, this, nullptr);

    ui->slider_seek->installEventFilter(this);
    ui->slider_seek->setTracking(true);
    ui->slider_seek->setMouseTracking(true);
    ui->slider_seek->setRange(0, 0);
    ui->slider_seek->setMinimumHeight(44);
    ui->slider_seek->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height: 8px; border-radius: 4px; background: rgba(255,255,255,72); }"
        "QSlider::sub-page:horizontal { height: 8px; border-radius: 4px; background: #22d3ee; }"
        "QSlider::handle:horizontal { width: 30px; height: 30px; margin: -11px 0; border-radius: 15px;"
        " background: white; border: 2px solid #22d3ee; }"));

    ui->btn_play->setText(QString());
    ui->btn_play->setFlat(true);

    connectControls();
    updatePlayButton();
    updateResponsiveLayout();
    QTimer::singleShot(0, this, &VideoPlayer::startPlayback);
}

VideoPlayer::~VideoPlayer()
{
    setPlaying(false);
    if (capture.isOpened())
    {
        capture.release();
    }
    delete ui;
}

void VideoPlayer::connectControls()
{
    connect(ui->sketchpad_back, &QPushButton::clicked,
            this, &VideoPlayer::on_sketchpad_back_clicked);
    connect(ui->btn_play, &QPushButton::clicked,
            this, &VideoPlayer::on_btn_play_clicked);
    connect(ui->slider_seek, &QSlider::sliderMoved,
            this, &VideoPlayer::on_slider_seek_sliderMoved);
    connect(ui->slider_seek, &QSlider::sliderPressed, this, [this]() {
        seeking = true;
        resumeAfterSeek = playing;
        setPlaying(false);
    });
    connect(ui->slider_seek, &QSlider::sliderReleased, this, [this]() {
        seekToSliderValue(ui->slider_seek->value());
        seeking = false;
        if (resumeAfterSeek && !playbackEnded)
        {
            setPlaying(true);
        }
        updatePlayButton();
    });
    frameTimer.setSingleShot(true);
    connect(&frameTimer, &QTimer::timeout, this, &VideoPlayer::renderNextFrame);
}

void VideoPlayer::startPlayback()
{
    qDebug() << "video player opening:" << videoPath;
    statusText = QStringLiteral("加载视频...");
    playbackEnded = false;
    currentFrameIndex = -1;
    durationMs = 0;
    frameTimesMs.clear();
    lastFrameTimestampMs = -1.0;
    ui->slider_seek->setRange(0, 0);
    ui->slider_seek->setValue(0);
    update();

    if (!openCapture())
    {
        statusText = QStringLiteral("视频打开失败");
        qWarning() << "video player open failed:" << videoPath;
        update();
        return;
    }

    if (!usingMppDecoder)
    {
        seekToSliderValue(0);
    }
    setPlaying(true);
}

bool VideoPlayer::openCapture()
{
    if (capture.isOpened())
    {
        capture.release();
    }

    usingMppDecoder = false;
    captureOpened = false;
    if (preferMppDecoder() && isH264Mp4Candidate(videoPath))
    {
        const QString pipeline = mppDecodePipeline(videoPath);
        captureOpened = capture.open(pipeline.toStdString(), cv::CAP_GSTREAMER);
        usingMppDecoder = captureOpened;
        if (!captureOpened)
        {
            qWarning() << "mpp video decode pipeline open failed, fallback to OpenCV:" << videoPath;
        }
    }

    if (!captureOpened)
    {
        captureOpened = capture.open(videoPath.toStdString());
        usingMppDecoder = false;
    }

    if (!captureOpened)
    {
        return false;
    }

    const VideoMetadata metadata = readVideoMetadata(videoPath);
    const double captureFps = capture.get(cv::CAP_PROP_FPS);
    const double fps = metadata.fps > 0.0 ? metadata.fps : captureFps;
    const int playbackFps = playbackFpsFromMetadata(fps);
    frameIntervalMs = qBound(kMinFrameDelayMs, static_cast<int>(1000.0 / playbackFps + 0.5), kMaxFrameDelayMs);

    const double captureCount = capture.get(cv::CAP_PROP_FRAME_COUNT);
    const int metadataFrames = metadata.frameCount > 0
                                   ? metadata.frameCount
                                   : (captureCount > 0.0 ? static_cast<int>(captureCount + 0.5) : 0);
    frameTimesMs = usingMppDecoder ? QVector<int>() : buildFrameTimelineMs(videoPath, frameIntervalMs);
    totalFrames = !frameTimesMs.isEmpty() ? frameTimesMs.size() : metadataFrames;
    durationMs = !frameTimesMs.isEmpty() ? frameTimesMs.back()
                                         : qMax(0, (totalFrames - 1) * frameIntervalMs);
    ui->slider_seek->setRange(0, qMax(0, durationMs));
    qDebug() << "video player metadata fps=" << fps
             << "metadata_frames=" << metadataFrames
             << "timeline_frames=" << frameTimesMs.size()
             << "duration_ms=" << durationMs
             << "preview_fps=" << playbackFps
             << "interval_ms=" << frameIntervalMs
             << "decoder=" << (usingMppDecoder ? "gstreamer-mpp" : "opencv");
    return true;
}

void VideoPlayer::scheduleNextFrame(int delayMs)
{
    if (playing && captureOpened)
    {
        frameTimer.start(qBound(kMinFrameDelayMs, delayMs, kMaxFrameDelayMs));
    }
}

void VideoPlayer::renderNextFrame()
{
    if (!captureOpened || !capture.isOpened())
    {
        finishPlayback();
        return;
    }

    cv::Mat frame;
    if (!capture.read(frame) || frame.empty())
    {
        finishPlayback();
        return;
    }

    currentFrameIndex = qMax(0, currentFrameIndex + 1);
    const int currentTimeMs = frameTimeMs(currentFrameIndex);
    const int nextTimeMs = frameTimeMs(currentFrameIndex + 1);
    const int nextDelayMs = nextTimeMs > currentTimeMs
                                ? qBound(kMinFrameDelayMs, nextTimeMs - currentTimeMs, kMaxFrameDelayMs)
                                : delayFromTimestamps(lastFrameTimestampMs,
                                                      static_cast<double>(currentTimeMs),
                                                      frameIntervalMs);
    lastFrameTimestampMs = currentTimeMs;
    showFrame(frame);

    if (!seeking && !ui->slider_seek->isSliderDown())
    {
        ui->slider_seek->setValue(qBound(ui->slider_seek->minimum(),
                                         currentTimeMs,
                                         ui->slider_seek->maximum()));
    }

    scheduleNextFrame(nextDelayMs);
}

void VideoPlayer::showFrame(const cv::Mat &frame)
{
    currentFrame = imageFromFrame(frame);
    statusText.clear();
    update(videoRect);
}

QImage VideoPlayer::imageFromFrame(const cv::Mat &frame) const
{
    cv::Mat rgb;
    if (frame.channels() == 4)
    {
        cv::cvtColor(frame, rgb, cv::COLOR_BGRA2RGBA);
        return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGBA8888).copy();
    }
    if (frame.channels() == 1)
    {
        cv::cvtColor(frame, rgb, cv::COLOR_GRAY2RGB);
    }
    else
    {
        cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
    }
    return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
}

void VideoPlayer::finishPlayback()
{
    setPlaying(false);
    playbackEnded = true;
    if (ui->slider_seek->maximum() > 0)
    {
        ui->slider_seek->setValue(ui->slider_seek->maximum());
        currentFrameIndex = qMax(0, totalFrames - 1);
    }
    updatePlayButton();
}

void VideoPlayer::setPlaying(bool enabled)
{
    playing = enabled && captureOpened;
    if (playing)
    {
        scheduleNextFrame(frameIntervalMs);
    }
    else
    {
        frameTimer.stop();
    }
    updatePlayButton();
}

void VideoPlayer::on_sketchpad_back_clicked()
{
    setPlaying(false);
    close();
}

void VideoPlayer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void VideoPlayer::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0));

    if (!currentFrame.isNull())
    {
        const QSize scaledSize = currentFrame.size().scaled(videoRect.size(), Qt::KeepAspectRatio);
        const QRect target(QPoint(videoRect.x() + (videoRect.width() - scaledSize.width()) / 2,
                                  videoRect.y() + (videoRect.height() - scaledSize.height()) / 2),
                           scaledSize);
        painter.drawImage(target, currentFrame);
    }
    else if (!statusText.isEmpty())
    {
        painter.setPen(Qt::white);
        painter.drawText(videoRect, Qt::AlignCenter, statusText);
    }
}

void VideoPlayer::updateResponsiveLayout()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
    {
        return;
    }

    const int barHeight = qBound(52, h / 8, 72);
    const int margin = qBound(8, qMin(w, h) / 50, 14);
    const int buttonSize = qBound(42, barHeight - margin, 56);
    const int y = h - barHeight + (barHeight - buttonSize) / 2;
    const int sliderHeight = qBound(44, barHeight - margin, 56);
    const int sliderX = margin * 2 + buttonSize;
    const int sliderY = h - barHeight + (barHeight - sliderHeight) / 2;

    videoRect = QRect(0, 0, w, qMax(1, h - barHeight));
    ui->sketchpad_back->setGeometry(w - margin - buttonSize, margin, buttonSize, buttonSize);
    ui->btn_play->setGeometry(margin, y, buttonSize, buttonSize);
    ui->btn_play->setIconSize(QSize(buttonSize - 8, buttonSize - 8));
    ui->slider_seek->setGeometry(sliderX, sliderY, qMax(140, w - sliderX - margin), sliderHeight);

    ui->sketchpad_back->raise();
    ui->btn_play->raise();
    ui->slider_seek->raise();
    update();
}

void VideoPlayer::on_btn_play_clicked()
{
    if (!captureOpened)
    {
        return;
    }

    if (playing)
    {
        setPlaying(false);
        return;
    }

    if (playbackEnded || (totalFrames > 0 && currentFrameIndex >= totalFrames - 1))
    {
        playbackEnded = false;
        seekToSliderValue(0);
    }
    setPlaying(true);
}

void VideoPlayer::updatePlayButton()
{
    ui->btn_play->setIcon(QIcon(AppPaths::existingDesktopFile(playing ? "images/start.svg"
                                                                      : "images/pause.svg")));
}

void VideoPlayer::on_slider_seek_sliderMoved(int position)
{
    seeking = true;
    setSliderPreviewValue(position);
}

void VideoPlayer::setSliderPreviewValue(int value)
{
    value = qBound(ui->slider_seek->minimum(), value, ui->slider_seek->maximum());
    ui->slider_seek->setValue(value);
    playbackEnded = durationMs > 0 && value >= durationMs;
}

void VideoPlayer::seekToSliderValue(int value)
{
    if (!captureOpened || !capture.isOpened())
    {
        return;
    }

    value = qBound(ui->slider_seek->minimum(), value, ui->slider_seek->maximum());
    ui->slider_seek->setValue(value);
    playbackEnded = durationMs > 0 && value >= durationMs;

    const int targetFrame = frameIndexForTimeMs(value);
    cv::Mat frame;
    if (usingMppDecoder)
    {
        capture.release();
        captureOpened = capture.open(mppDecodePipeline(videoPath).toStdString(), cv::CAP_GSTREAMER);
        if (!captureOpened)
        {
            usingMppDecoder = false;
            captureOpened = capture.open(videoPath.toStdString());
            if (!captureOpened)
            {
                return;
            }
            capture.set(cv::CAP_PROP_POS_FRAMES, targetFrame);
        }
        else
        {
            for (int i = 0; i <= targetFrame; ++i)
            {
                if (!capture.read(frame) || frame.empty())
                {
                    break;
                }
            }
            if (!frame.empty())
            {
                currentFrameIndex = targetFrame;
                lastFrameTimestampMs = frameTimeMs(currentFrameIndex);
                showFrame(frame);
                return;
            }
        }
    }

    bool positioned = capture.set(cv::CAP_PROP_POS_FRAMES, targetFrame);
    if (!positioned)
    {
        capture.set(cv::CAP_PROP_POS_FRAMES, targetFrame);
    }
    if (capture.read(frame) && !frame.empty())
    {
        currentFrameIndex = targetFrame;
        lastFrameTimestampMs = frameTimeMs(currentFrameIndex);
        showFrame(frame);
    }
}

int VideoPlayer::frameTimeMs(int frameIndex) const
{
    if (!frameTimesMs.isEmpty())
    {
        const int index = qBound(0, frameIndex, frameTimesMs.size() - 1);
        return frameTimesMs[index];
    }
    return qMax(0, frameIndex * frameIntervalMs);
}

int VideoPlayer::frameIndexForTimeMs(int timeMs) const
{
    if (frameTimesMs.isEmpty())
    {
        return frameIntervalMs > 0 ? qMax(0, timeMs / frameIntervalMs) : 0;
    }

    const auto it = std::lower_bound(frameTimesMs.constBegin(), frameTimesMs.constEnd(), timeMs);
    if (it == frameTimesMs.constEnd())
    {
        return frameTimesMs.size() - 1;
    }
    if (it == frameTimesMs.constBegin())
    {
        return 0;
    }

    const int upperIndex = static_cast<int>(it - frameTimesMs.constBegin());
    const int lowerIndex = upperIndex - 1;
    return (timeMs - frameTimesMs[lowerIndex] <= frameTimesMs[upperIndex] - timeMs)
               ? lowerIndex
               : upperIndex;
}

int VideoPlayer::sliderValueFromPosition(const QPoint &pos) const
{
    const int span = qMax(1, ui->slider_seek->width());
    const int pixelPos = qBound(0, pos.x(), span);
    return QStyle::sliderValueFromPosition(ui->slider_seek->minimum(),
                                           ui->slider_seek->maximum(),
                                           pixelPos,
                                           span);
}

bool VideoPlayer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != ui->slider_seek)
    {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress)
    {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton)
        {
            seeking = true;
            resumeAfterSeek = playing;
            setPlaying(false);
            setSliderPreviewValue(sliderValueFromPosition(mouseEvent->pos()));
            return true;
        }
    }
    else if (event->type() == QEvent::MouseMove)
    {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (seeking || (mouseEvent->buttons() & Qt::LeftButton))
        {
            seeking = true;
            setSliderPreviewValue(sliderValueFromPosition(mouseEvent->pos()));
            return true;
        }
    }
    else if (event->type() == QEvent::MouseButtonRelease)
    {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (seeking && mouseEvent->button() == Qt::LeftButton)
        {
            seekToSliderValue(sliderValueFromPosition(mouseEvent->pos()));
            seeking = false;
            if (resumeAfterSeek && !playbackEnded)
            {
                setPlaying(true);
            }
            updatePlayButton();
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}
