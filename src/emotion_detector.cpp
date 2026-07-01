#include "emotion_detector.h"

#include "app_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "rknn_api.h"

namespace serial_motor
{
namespace
{

constexpr int kEmotionClassCount = 7;
constexpr int kYoloAttrCount = 5 + kEmotionClassCount;

const char *kEmotionLabelsCn[kEmotionClassCount] = {
    "愤怒", "厌恶", "恐惧", "高兴", "中性", "伤心", "惊讶",
};

const char *kEmotionLabelsEn[kEmotionClassCount] = {
    "angry", "disgust", "fear", "happy", "neutral", "sad", "surprise",
};

using xiaoman::cwd_and_parents;
using xiaoman::join_path;
using xiaoman::path_exists;

void append_unique(std::vector<std::string> *items, const std::string &value)
{
    if (value.empty() || std::find(items->begin(), items->end(), value) != items->end())
    {
        return;
    }
    items->push_back(value);
}

std::vector<std::string> find_model_candidates(const EmotionDetectorOptions &options)
{
    std::vector<std::string> candidates;
    if (!options.model_path.empty() && path_exists(options.model_path))
    {
        append_unique(&candidates, options.model_path);
    }

    const char *env_path = std::getenv("XIAOMAN_EMOTION_MODEL");
    if (env_path != nullptr && env_path[0] != '\0' && path_exists(env_path))
    {
        append_unique(&candidates, env_path);
    }

    env_path = std::getenv("NEWBOT_EMOTION_MODEL");
    if (env_path != nullptr && env_path[0] != '\0' && path_exists(env_path))
    {
        append_unique(&candidates, env_path);
    }

    std::vector<std::string> model_names;
    append_unique(&model_names, options.model_name);
    const std::vector<std::string> fallback_model_names{
        "face_emotion/face_emotion_yolov5_rk3566_640x640_toolkit160.rknn",
        "face_emotion/face_emotion_yolov5_rk3566_640x640_toolkit152.rknn",
        "face_emotion/face_emotion_yolov5_rk3566_640x640.rknn",
    };
    for (const std::string &name : fallback_model_names)
    {
        append_unique(&model_names, name);
    }

    for (const std::string &model_name : model_names)
    {
        append_unique(&candidates, join_path("models", model_name));
        append_unique(&candidates, join_path(join_path("..", "models"), model_name));
        append_unique(&candidates, join_path("tools/serial_motor_control/models", model_name));
    }

    for (const std::string &dir : cwd_and_parents(6))
    {
        for (const std::string &model_name : model_names)
        {
            append_unique(&candidates, join_path(join_path(dir, "models"), model_name));
            append_unique(&candidates, join_path(join_path(dir, "friday_voice_speaker/models"), model_name));
            append_unique(&candidates, join_path(join_path(dir, "serial_motor_control/models"), model_name));
            append_unique(&candidates, join_path(join_path(dir, "tools/serial_motor_control/models"), model_name));
        }
    }

    const char *home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0')
    {
        const std::string cpp_dir = join_path(home, "cpp");
        for (const std::string &model_name : model_names)
        {
            append_unique(&candidates, join_path(join_path(cpp_dir, "tools/serial_motor_control/models"), model_name));
            append_unique(&candidates, join_path(join_path(cpp_dir, "newbot-master/tools/serial_motor_control/models"), model_name));
            append_unique(&candidates, join_path(join_path(cpp_dir, "friday_voice_speaker/models"), model_name));
        }
    }
    for (const std::string &model_name : model_names)
    {
        append_unique(&candidates, join_path("/home/orangepi/cpp/tools/serial_motor_control/models", model_name));
        append_unique(&candidates, join_path("/home/orangepi/cpp/friday_voice_speaker/models", model_name));
    }

    std::vector<std::string> existing;
    for (const std::string &candidate : candidates)
    {
        if (path_exists(candidate))
        {
            append_unique(&existing, candidate);
        }
    }
    return existing;
}

float sigmoid(float value)
{
    return 1.0f / (1.0f + std::exp(-value));
}

bool looks_like_probability(float value)
{
    return value >= 0.0f && value <= 1.0f;
}

float normalize_score(float value)
{
    return looks_like_probability(value) ? value : sigmoid(value);
}

float detection_iou(const EmotionDetection &a, const EmotionDetection &b)
{
    const int x1 = std::max(a.x1, b.x1);
    const int y1 = std::max(a.y1, b.y1);
    const int x2 = std::min(a.x2, b.x2);
    const int y2 = std::min(a.y2, b.y2);
    const int w = std::max(0, x2 - x1 + 1);
    const int h = std::max(0, y2 - y1 + 1);
    const float intersection = static_cast<float>(w * h);
    const float area_a = static_cast<float>(
        std::max(0, a.x2 - a.x1 + 1) * std::max(0, a.y2 - a.y1 + 1));
    const float area_b = static_cast<float>(
        std::max(0, b.x2 - b.x1 + 1) * std::max(0, b.y2 - b.y1 + 1));
    const float union_area = area_a + area_b - intersection;
    return union_area <= 0.0f ? 0.0f : intersection / union_area;
}

std::vector<EmotionDetection> nms_emotion_detections(std::vector<EmotionDetection> detections,
                                                     float threshold)
{
    std::sort(detections.begin(), detections.end(),
              [](const EmotionDetection &a, const EmotionDetection &b)
              {
                  return a.confidence > b.confidence;
              });

    std::vector<EmotionDetection> kept;
    std::vector<bool> removed(detections.size(), false);
    for (size_t i = 0; i < detections.size(); ++i)
    {
        if (removed[i])
        {
            continue;
        }
        kept.push_back(detections[i]);
        for (size_t j = i + 1; j < detections.size(); ++j)
        {
            if (!removed[j] && detection_iou(detections[i], detections[j]) > threshold)
            {
                removed[j] = true;
            }
        }
    }
    return kept;
}

struct LetterboxInfo
{
    float scale = 1.0f;
    float pad_x = 0.0f;
    float pad_y = 0.0f;
};

cv::Mat letterbox_rgb(const cv::Mat &bgr_frame,
                      int target_width,
                      int target_height,
                      LetterboxInfo *info,
                      cv::Mat *bgr_scratch,
                      cv::Mat *resized_scratch,
                      cv::Mat *canvas_scratch,
                      cv::Mat *rgb_scratch)
{
    cv::Mat bgr;
    if (bgr_frame.channels() == 3)
    {
        bgr = bgr_frame;
    }
    else if (bgr_frame.channels() == 4)
    {
        cv::cvtColor(bgr_frame, *bgr_scratch, cv::COLOR_BGRA2BGR);
        bgr = *bgr_scratch;
    }
    else if (bgr_frame.channels() == 1)
    {
        cv::cvtColor(bgr_frame, *bgr_scratch, cv::COLOR_GRAY2BGR);
        bgr = *bgr_scratch;
    }
    else
    {
        throw std::runtime_error("unsupported image channel count");
    }

    const float scale = std::min(target_width / static_cast<float>(bgr.cols),
                                 target_height / static_cast<float>(bgr.rows));
    const int resized_width = std::max(1, static_cast<int>(std::round(bgr.cols * scale)));
    const int resized_height = std::max(1, static_cast<int>(std::round(bgr.rows * scale)));
    const int pad_x = (target_width - resized_width) / 2;
    const int pad_y = (target_height - resized_height) / 2;

    cv::resize(bgr, *resized_scratch, cv::Size(resized_width, resized_height));

    canvas_scratch->create(target_height, target_width, CV_8UC3);
    canvas_scratch->setTo(cv::Scalar(114, 114, 114));
    resized_scratch->copyTo((*canvas_scratch)(cv::Rect(pad_x, pad_y, resized_width, resized_height)));

    cv::cvtColor(*canvas_scratch, *rgb_scratch, cv::COLOR_BGR2RGB);

    if (info != nullptr)
    {
        info->scale = scale;
        info->pad_x = static_cast<float>(pad_x);
        info->pad_y = static_cast<float>(pad_y);
    }
    return *rgb_scratch;
}

} // namespace

struct EmotionDetector::Impl
{
    explicit Impl(EmotionDetectorOptions detector_options) : options(std::move(detector_options))
    {
        const std::vector<std::string> model_candidates = find_model_candidates(options);
        if (model_candidates.empty())
        {
            std::cerr << "warning: emotion RKNN model not found; put "
                      << options.model_name
                      << " under friday_voice_speaker/models\n";
            return;
        }

        for (const std::string &model_path : model_candidates)
        {
            std::cout << "trying emotion RKNN model: " << model_path << "\n";
            if (init(model_path))
            {
                available = true;
                std::cout << "RKNN emotion detector loaded: " << model_path
                          << " input=" << model_width << "x" << model_height << "\n";
                return;
            }
            release();
            std::cerr << "warning: emotion RKNN model rejected, trying next candidate: "
                      << model_path << "\n";
        }

        std::cerr << "emotion: no compatible RKNN emotion model loaded\n";
    }

    ~Impl()
    {
        release();
    }

    std::vector<EmotionDetection> detect(const cv::Mat &bgr_frame)
    {
        if (!available || bgr_frame.empty())
        {
            return {};
        }

        LetterboxInfo letterbox;
        cv::Mat rgb = letterbox_rgb(bgr_frame,
                                    model_width,
                                    model_height,
                                    &letterbox,
                                    &bgr_scratch,
                                    &resized_scratch,
                                    &canvas_scratch,
                                    &rgb_scratch);
        if (!rgb.isContinuous())
        {
            rgb = rgb.clone();
        }

        rknn_input input{};
        input.index = 0;
        input.type = RKNN_TENSOR_UINT8;
        input.size = static_cast<uint32_t>(model_width * model_height * model_channel);
        input.fmt = RKNN_TENSOR_NHWC;
        input.pass_through = 0;
        input.buf = rgb.data;

        int ret = rknn_inputs_set(ctx, io_num.n_input, &input);
        if (ret < 0)
        {
            std::cerr << "warning: emotion rknn_inputs_set failed: " << ret << "\n";
            return {};
        }

        ret = rknn_run(ctx, nullptr);
        if (ret < 0)
        {
            std::cerr << "warning: emotion rknn_run failed: " << ret << "\n";
            return {};
        }

        if (outputs.size() != io_num.n_output)
        {
            outputs.resize(io_num.n_output);
        }
        for (auto &output : outputs)
        {
            std::memset(&output, 0, sizeof(output));
            output.want_float = 1;
        }

        ret = rknn_outputs_get(ctx, io_num.n_output, outputs.data(), nullptr);
        if (ret < 0)
        {
            std::cerr << "warning: emotion rknn_outputs_get failed: " << ret << "\n";
            return {};
        }

        std::vector<EmotionDetection> detections;
        try
        {
            detections = postprocess(outputs, bgr_frame.cols, bgr_frame.rows, letterbox);
        }
        catch (const std::exception &e)
        {
            std::cerr << "warning: emotion YOLOv5 postprocess failed: " << e.what() << "\n";
        }

        rknn_outputs_release(ctx, io_num.n_output, outputs.data());
        return detections;
    }

    void release()
    {
        if (ctx != 0)
        {
            rknn_destroy(ctx);
            ctx = 0;
        }
        available = false;
    }

    bool init(const std::string &model_path)
    {
        std::ifstream in(model_path, std::ios::binary);
        if (!in)
        {
            std::cerr << "warning: open emotion RKNN model failed: " << model_path << "\n";
            return false;
        }

        std::vector<uint8_t> model_data(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        if (model_data.empty())
        {
            std::cerr << "warning: empty emotion RKNN model: " << model_path << "\n";
            return false;
        }

        int ret = rknn_init(&ctx, model_data.data(), static_cast<uint32_t>(model_data.size()), 0, nullptr);
        if (ret < 0)
        {
            std::cerr << "warning: emotion rknn_init failed: " << ret << "\n";
            return false;
        }

        ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        if (ret < 0 || io_num.n_input < 1 || io_num.n_output < 1)
        {
            std::cerr << "warning: invalid emotion RKNN io num, ret=" << ret
                      << " input=" << io_num.n_input
                      << " output=" << io_num.n_output << "\n";
            return false;
        }

        input_attrs.assign(io_num.n_input, rknn_tensor_attr{});
        for (uint32_t i = 0; i < io_num.n_input; ++i)
        {
            input_attrs[i].index = i;
            ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
            if (ret < 0)
            {
                std::cerr << "warning: query emotion RKNN input attr failed: " << ret << "\n";
                return false;
            }
        }

        output_attrs.assign(io_num.n_output, rknn_tensor_attr{});
        for (uint32_t i = 0; i < io_num.n_output; ++i)
        {
            output_attrs[i].index = i;
            ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
            if (ret < 0)
            {
                std::cerr << "warning: query emotion RKNN output attr failed: " << ret << "\n";
                return false;
            }
        }

        const rknn_tensor_attr &input = input_attrs[0];
        if (input.fmt == RKNN_TENSOR_NCHW)
        {
            model_channel = input.dims[1];
            model_height = input.dims[2];
            model_width = input.dims[3];
        }
        else
        {
            model_height = input.dims[1];
            model_width = input.dims[2];
            model_channel = input.dims[3];
        }

        std::cout << "emotion RKNN io: inputs=" << io_num.n_input
                  << " outputs=" << io_num.n_output << "\n";
        for (const rknn_tensor_attr &attr : output_attrs)
        {
            std::cout << "  output" << attr.index << " dims=";
            for (uint32_t i = 0; i < attr.n_dims; ++i)
            {
                std::cout << (i == 0 ? "" : "x") << attr.dims[i];
            }
            std::cout << " elems=" << attr.n_elems << "\n";
        }

        return model_width > 0 && model_height > 0 && model_channel == 3;
    }

    std::vector<EmotionDetection> postprocess(const std::vector<rknn_output> &outputs,
                                              int image_width,
                                              int image_height,
                                              const LetterboxInfo &letterbox)
    {
        if (outputs.empty() || outputs[0].buf == nullptr || output_attrs.empty())
        {
            return {};
        }

        const float *data = static_cast<const float *>(outputs[0].buf);
        const rknn_tensor_attr &attr = output_attrs[0];
        if (attr.n_elems == 0 || attr.n_elems % kYoloAttrCount != 0)
        {
            std::cerr << "warning: unsupported emotion output shape; expected Nx"
                      << kYoloAttrCount << ", elems=" << attr.n_elems << "\n";
            return {};
        }

        std::vector<EmotionDetection> detections;
        const bool row_major = attr.n_dims > 0 && attr.dims[attr.n_dims - 1] == kYoloAttrCount;
        const uint32_t rows = attr.n_elems / kYoloAttrCount;

        for (uint32_t i = 0; i < rows; ++i)
        {
            float values[kYoloAttrCount]{};
            if (row_major)
            {
                const float *row = data + i * kYoloAttrCount;
                std::copy(row, row + kYoloAttrCount, values);
            }
            else
            {
                // Some RKNN versions expose YOLO output as [1, attr, rows].
                for (int j = 0; j < kYoloAttrCount; ++j)
                {
                    values[j] = data[j * rows + i];
                }
            }

            const float objectness = normalize_score(values[4]);
            if (objectness < 0.01f)
            {
                continue;
            }

            int best_class = 0;
            float best_class_score = normalize_score(values[5]);
            for (int class_id = 1; class_id < kEmotionClassCount; ++class_id)
            {
                const float class_score = normalize_score(values[5 + class_id]);
                if (class_score > best_class_score)
                {
                    best_class = class_id;
                    best_class_score = class_score;
                }
            }

            const float confidence = objectness * best_class_score;
            if (confidence < options.confidence_threshold)
            {
                continue;
            }

            const float cx = values[0];
            const float cy = values[1];
            const float w = values[2];
            const float h = values[3];

            float x1 = cx - w * 0.5f;
            float y1 = cy - h * 0.5f;
            float x2 = cx + w * 0.5f;
            float y2 = cy + h * 0.5f;

            x1 = (x1 - letterbox.pad_x) / letterbox.scale;
            x2 = (x2 - letterbox.pad_x) / letterbox.scale;
            y1 = (y1 - letterbox.pad_y) / letterbox.scale;
            y2 = (y2 - letterbox.pad_y) / letterbox.scale;

            EmotionDetection det;
            det.x1 = std::max(0, std::min(image_width - 1, static_cast<int>(std::round(x1))));
            det.y1 = std::max(0, std::min(image_height - 1, static_cast<int>(std::round(y1))));
            det.x2 = std::max(0, std::min(image_width - 1, static_cast<int>(std::round(x2))));
            det.y2 = std::max(0, std::min(image_height - 1, static_cast<int>(std::round(y2))));
            det.class_id = best_class;
            det.confidence = confidence;
            det.label_cn = kEmotionLabelsCn[best_class];
            det.label_en = kEmotionLabelsEn[best_class];

            if (det.x2 > det.x1 && det.y2 > det.y1)
            {
                detections.push_back(det);
            }
        }

        return nms_emotion_detections(detections, options.nms_threshold);
    }

    rknn_context ctx = 0;
    rknn_input_output_num io_num{};
    std::vector<rknn_tensor_attr> input_attrs;
    std::vector<rknn_tensor_attr> output_attrs;
    std::vector<rknn_output> outputs;
    cv::Mat bgr_scratch;
    cv::Mat resized_scratch;
    cv::Mat canvas_scratch;
    cv::Mat rgb_scratch;
    int model_width = 0;
    int model_height = 0;
    int model_channel = 3;
    bool available = false;
    EmotionDetectorOptions options;
};

EmotionDetector::EmotionDetector(EmotionDetectorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

EmotionDetector::~EmotionDetector() = default;

bool EmotionDetector::available() const
{
    return impl_ != nullptr && impl_->available;
}

std::vector<EmotionDetection> EmotionDetector::detect(const cv::Mat &bgr_frame)
{
    if (impl_ == nullptr)
    {
        return {};
    }
    return impl_->detect(bgr_frame);
}

void draw_emotion_detections(cv::Mat &frame, const std::vector<EmotionDetection> &detections)
{
    for (const EmotionDetection &det : detections)
    {
        const cv::Scalar color(0, 220, 255);
        cv::rectangle(frame,
                      cv::Point(det.x1, det.y1),
                      cv::Point(det.x2, det.y2),
                      color,
                      2);

        std::ostringstream label;
        label.setf(std::ios::fixed);
        label.precision(2);
        label << det.label_en << " " << det.confidence;

        int baseline = 0;
        const cv::Size text_size = cv::getTextSize(label.str(), cv::FONT_HERSHEY_SIMPLEX, 0.55, 2, &baseline);
        const int label_x = std::max(0, det.x1);
        const int label_y = std::max(text_size.height + 4, det.y1 - 4);
        cv::rectangle(frame,
                      cv::Point(label_x, label_y - text_size.height - 4),
                      cv::Point(label_x + text_size.width + 6, label_y + baseline),
                      color,
                      cv::FILLED);
        cv::putText(frame,
                    label.str(),
                    cv::Point(label_x + 3, label_y - 2),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.55,
                    cv::Scalar(0, 0, 0),
                    2,
                    cv::LINE_AA);
    }
}

} // namespace serial_motor
