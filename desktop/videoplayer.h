#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <QImage>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

class QEvent;
class QMouseEvent;
class QPoint;
class QPaintEvent;
class QResizeEvent;

namespace Ui {
class VideoPlayer;
}

class VideoPlayer : public QWidget
{
    Q_OBJECT

public:
    explicit VideoPlayer(QString videoPath, QWidget *parent = nullptr);
    ~VideoPlayer();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_sketchpad_back_clicked();
    void on_btn_play_clicked();
    void on_slider_seek_sliderMoved(int position);

private:
    void connectControls();
    void startPlayback();
    bool openCapture();
    void renderNextFrame();
    void scheduleNextFrame(int delayMs);
    void showFrame(const cv::Mat &frame);
    void updateResponsiveLayout();
    void updatePlayButton();
    void setPlaying(bool enabled);
    void finishPlayback();
    void setSliderPreviewValue(int value);
    void seekToSliderValue(int value);
    int frameTimeMs(int frameIndex) const;
    int frameIndexForTimeMs(int timeMs) const;
    int sliderValueFromPosition(const QPoint &pos) const;
    QImage imageFromFrame(const cv::Mat &frame) const;

    Ui::VideoPlayer *ui = nullptr;
    cv::VideoCapture capture;
    QString videoPath;
    QTimer frameTimer;
    QImage currentFrame;
    QRect videoRect;
    QString statusText;
    QVector<int> frameTimesMs;
    int totalFrames = 0;
    int currentFrameIndex = 0;
    int durationMs = 0;
    int frameIntervalMs = 40;
    double lastFrameTimestampMs = -1.0;
    bool captureOpened = false;
    bool playing = false;
    bool seeking = false;
    bool resumeAfterSeek = false;
    bool playbackEnded = false;
    bool usingMppDecoder = false;
};

#endif // VIDEOPLAYER_H
