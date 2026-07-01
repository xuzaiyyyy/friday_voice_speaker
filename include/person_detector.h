#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace serial_motor
{

struct PersonDetection
{
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    float confidence = 0.0f;
};

struct PersonDetectorOptions
{
    std::string model_path;
    std::string model_name = "yolov6n_85.rknn";
    float confidence_threshold = 0.55f;
    float nms_threshold = 0.20f;
};

class PersonDetector
{
public:
    explicit PersonDetector(PersonDetectorOptions options = {});
    ~PersonDetector();

    PersonDetector(const PersonDetector &) = delete;
    PersonDetector &operator=(const PersonDetector &) = delete;

    bool available() const;
    std::vector<PersonDetection> detect(const cv::Mat &bgr_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void draw_person_detections(cv::Mat &frame, const std::vector<PersonDetection> &detections);

} // namespace serial_motor
