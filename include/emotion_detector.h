#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace serial_motor
{

struct EmotionDetection
{
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int class_id = -1;
    float confidence = 0.0f;
    std::string label_cn;
    std::string label_en;
};

struct EmotionDetectorOptions
{
    std::string model_path;
    std::string model_name = "face_emotion/face_emotion_yolov5_rk3566_640x640_toolkit160.rknn";
    float confidence_threshold = 0.35f;
    float nms_threshold = 0.45f;
};

class EmotionDetector
{
public:
    explicit EmotionDetector(EmotionDetectorOptions options = {});
    ~EmotionDetector();

    EmotionDetector(const EmotionDetector &) = delete;
    EmotionDetector &operator=(const EmotionDetector &) = delete;

    bool available() const;
    std::vector<EmotionDetection> detect(const cv::Mat &bgr_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void draw_emotion_detections(cv::Mat &frame, const std::vector<EmotionDetection> &detections);

} // namespace serial_motor
