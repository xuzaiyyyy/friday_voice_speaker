#include "person_detector.h"

#include "app_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>

#include <opencv2/imgproc.hpp>

#ifdef SMART_SPEAKER_USE_RKNN_PERSON
#include "rknn_api.h"
#endif

namespace serial_motor
{
namespace
{

constexpr int kCocoClassCount = 80;
constexpr int kPersonClassId = 0;
constexpr int kClassScoreOffset = 5;

using xiaoman::cwd_and_parents;
using xiaoman::join_path;
using xiaoman::path_exists;

std::string find_model_path(const PersonDetectorOptions &options)
{
    if (!options.model_path.empty() && path_exists(options.model_path))
    {
        return options.model_path;
    }

    const char *env_path = std::getenv("XIAOMAN_YOLO_PERSON_MODEL");
    if (env_path != nullptr && env_path[0] != '\0' && path_exists(env_path))
    {
        return env_path;
    }
    env_path = std::getenv("NEWBOT_YOLO_PERSON_MODEL");
    if (env_path != nullptr && env_path[0] != '\0' && path_exists(env_path))
    {
        return env_path;
    }

    std::vector<std::string> candidates{
        join_path("models", options.model_name),
        join_path(join_path("..", "models"), options.model_name),
        join_path(join_path("friday_voice_speaker", "models"), options.model_name),
        join_path("tools/serial_motor_control/models", options.model_name),
    };

    for (const std::string &dir : cwd_and_parents(6))
    {
        candidates.push_back(join_path(join_path(dir, "models"), options.model_name));
        candidates.push_back(join_path(join_path(dir, "friday_voice_speaker/models"), options.model_name));
        candidates.push_back(join_path(join_path(dir, "serial_motor_control/models"), options.model_name));
        candidates.push_back(join_path(join_path(dir, "tools/serial_motor_control/models"), options.model_name));
    }

    const char *home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0')
    {
        const std::string cpp_dir = join_path(home, "cpp");
        candidates.push_back(join_path(join_path(cpp_dir, "friday_voice_speaker/models"), options.model_name));
        candidates.push_back(join_path(join_path(cpp_dir, "tools/serial_motor_control/models"), options.model_name));
        candidates.push_back(join_path(join_path(cpp_dir, "newbot-master/tools/serial_motor_control/models"), options.model_name));
    }
    candidates.push_back(join_path("/home/orangepi/cpp/friday_voice_speaker/models", options.model_name));
    candidates.push_back(join_path("/home/orangepi/cpp/tools/serial_motor_control/models", options.model_name));

    for (const std::string &candidate : candidates)
    {
        if (path_exists(candidate))
        {
            return candidate;
        }
    }
    return "";
}

float detection_iou(const PersonDetection &a, const PersonDetection &b)
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

std::vector<PersonDetection> nms_person_detections(std::vector<PersonDetection> detections,
                                                   float threshold)
{
    std::sort(detections.begin(), detections.end(),
              [](const PersonDetection &a, const PersonDetection &b)
              {
                  return a.confidence > b.confidence;
              });

    std::vector<PersonDetection> kept;
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

} // namespace

struct PersonDetector::Impl
{
    explicit Impl(PersonDetectorOptions detector_options) : options(std::move(detector_options))
    {
#ifdef SMART_SPEAKER_USE_RKNN_PERSON
        const std::string model_path = find_model_path(options);
        if (model_path.empty())
        {
            std::cerr << "warning: YOLO person model not found; put "
                      << options.model_name
                      << " under friday_voice_speaker/models\n";
            return;
        }

        if (!init(model_path))
        {
            release();
            return;
        }

        available = true;
        std::cout << "RKNN YOLOv6 person detector loaded: " << model_path
                  << " input=" << model_width << "x" << model_height << "\n";
#else
        (void)options;
        std::cerr << "warning: RKNN person detection is not enabled at build time; "
                  << "camera preview will not draw person boxes.\n";
#endif
    }

    ~Impl()
    {
        release();
    }

    std::vector<PersonDetection> detect(const cv::Mat &bgr_frame)
    {
#ifdef SMART_SPEAKER_USE_RKNN_PERSON
        if (!available || bgr_frame.empty())
        {
            return {};
        }

        cv::Mat bgr;
        if (bgr_frame.channels() == 3)
        {
            bgr = bgr_frame;
        }
        else if (bgr_frame.channels() == 4)
        {
            cv::cvtColor(bgr_frame, bgr_scratch, cv::COLOR_BGRA2BGR);
            bgr = bgr_scratch;
        }
        else if (bgr_frame.channels() == 1)
        {
            cv::cvtColor(bgr_frame, bgr_scratch, cv::COLOR_GRAY2BGR);
            bgr = bgr_scratch;
        }
        else
        {
            return {};
        }

        cv::cvtColor(bgr, rgb_scratch, cv::COLOR_BGR2RGB);
        cv::resize(rgb_scratch, resized_scratch, cv::Size(model_width, model_height));
        if (!resized_scratch.isContinuous())
        {
            resized_scratch = resized_scratch.clone();
        }

        rknn_input input{};
        input.index = 0;
        input.type = RKNN_TENSOR_UINT8;
        input.size = static_cast<uint32_t>(model_width * model_height * model_channel);
        input.fmt = RKNN_TENSOR_NHWC;
        input.pass_through = 0;
        input.buf = resized_scratch.data;

        int ret = rknn_inputs_set(ctx, io_num.n_input, &input);
        if (ret < 0)
        {
            std::cerr << "warning: rknn_inputs_set failed: " << ret << "\n";
            return {};
        }

        ret = rknn_run(ctx, nullptr);
        if (ret < 0)
        {
            std::cerr << "warning: rknn_run failed: " << ret << "\n";
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
            std::cerr << "warning: rknn_outputs_get failed: " << ret << "\n";
            return {};
        }

        std::vector<PersonDetection> detections;
        try
        {
            detections = postprocess(outputs, bgr.cols, bgr.rows);
        }
        catch (const std::exception &e)
        {
            std::cerr << "warning: YOLO postprocess failed: " << e.what() << "\n";
        }

        rknn_outputs_release(ctx, io_num.n_output, outputs.data());
        return detections;
#else
        (void)bgr_frame;
        return {};
#endif
    }

    void release()
    {
#ifdef SMART_SPEAKER_USE_RKNN_PERSON
        if (ctx != 0)
        {
            rknn_destroy(ctx);
            ctx = 0;
        }
#endif
        available = false;
    }

#ifdef SMART_SPEAKER_USE_RKNN_PERSON
    bool init(const std::string &model_path)
    {
        std::ifstream in(model_path, std::ios::binary);
        if (!in)
        {
            std::cerr << "warning: open RKNN model failed: " << model_path << "\n";
            return false;
        }

        std::vector<uint8_t> model_data(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        if (model_data.empty())
        {
            std::cerr << "warning: empty RKNN model: " << model_path << "\n";
            return false;
        }

        int ret = rknn_init(&ctx, model_data.data(), static_cast<uint32_t>(model_data.size()), 0, nullptr);
        if (ret < 0)
        {
            std::cerr << "warning: rknn_init failed: " << ret << "\n";
            return false;
        }

        ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        if (ret < 0 || io_num.n_input < 1 || io_num.n_output < 3)
        {
            std::cerr << "warning: invalid RKNN io num, ret=" << ret
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
                std::cerr << "warning: query RKNN input attr failed: " << ret << "\n";
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
                std::cerr << "warning: query RKNN output attr failed: " << ret << "\n";
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
        return model_width > 0 && model_height > 0 && model_channel == 3;
    }

    std::vector<PersonDetection> postprocess(const std::vector<rknn_output> &outputs,
                                             int image_width,
                                             int image_height)
    {
        std::vector<PersonDetection> detections;
        const int output_count = std::min<int>(3, static_cast<int>(outputs.size()));
        for (int i = 0; i < output_count; ++i)
        {
            const int stride = (i == 0) ? 8 : ((i == 1) ? 16 : 32);
            const int grid_h = model_height / stride;
            const int grid_w = model_width / stride;
            const float *input = static_cast<const float *>(outputs[static_cast<size_t>(i)].buf);
            if (input == nullptr)
            {
                continue;
            }

            process_output(input, grid_h, grid_w, stride, detections);
        }

        for (PersonDetection &det : detections)
        {
            det.x1 = std::max(0, std::min(image_width - 1, static_cast<int>(det.x1 * image_width / static_cast<float>(model_width))));
            det.x2 = std::max(0, std::min(image_width - 1, static_cast<int>(det.x2 * image_width / static_cast<float>(model_width))));
            det.y1 = std::max(0, std::min(image_height - 1, static_cast<int>(det.y1 * image_height / static_cast<float>(model_height))));
            det.y2 = std::max(0, std::min(image_height - 1, static_cast<int>(det.y2 * image_height / static_cast<float>(model_height))));
        }
        return nms_person_detections(detections, options.nms_threshold);
    }

    void process_output(const float *input,
                        int grid_h,
                        int grid_w,
                        int stride,
                        std::vector<PersonDetection> &detections)
    {
        const int grid_len = grid_h * grid_w;
        for (int y = 0; y < grid_h; ++y)
        {
            for (int x = 0; x < grid_w; ++x)
            {
                const int offset = y * grid_w + x;
                const float object_gate = input[4 * grid_len + offset];
                if (object_gate < options.confidence_threshold)
                {
                    continue;
                }

                int best_class_id = 0;
                float best_class_conf = input[(kClassScoreOffset + kPersonClassId) * grid_len + offset];
                for (int class_id = 1; class_id < kCocoClassCount; ++class_id)
                {
                    const float class_conf = input[(kClassScoreOffset + class_id) * grid_len + offset];
                    if (class_conf > best_class_conf)
                    {
                        best_class_id = class_id;
                        best_class_conf = class_conf;
                    }
                }

                // COCO class 0 is "person"; ignore every other best class even if it has a box.
                if (best_class_id != kPersonClassId || best_class_conf < options.confidence_threshold)
                {
                    continue;
                }

                const float box_x1 = (x + 0.5f - input[offset]) * stride;
                const float box_y1 = (y + 0.5f - input[1 * grid_len + offset]) * stride;
                const float box_x2 = (x + 0.5f + input[2 * grid_len + offset]) * stride;
                const float box_y2 = (y + 0.5f + input[3 * grid_len + offset]) * stride;

                PersonDetection det;
                det.x1 = static_cast<int>(box_x1);
                det.y1 = static_cast<int>(box_y1);
                det.x2 = static_cast<int>(box_x2);
                det.y2 = static_cast<int>(box_y2);
                det.confidence = best_class_conf;
                detections.push_back(det);
            }
        }
    }

    rknn_context ctx = 0;
    rknn_input_output_num io_num{};
    std::vector<rknn_tensor_attr> input_attrs;
    std::vector<rknn_tensor_attr> output_attrs;
    std::vector<rknn_output> outputs;
    cv::Mat bgr_scratch;
    cv::Mat rgb_scratch;
    cv::Mat resized_scratch;
    int model_width = 0;
    int model_height = 0;
    int model_channel = 3;
#endif

    bool available = false;
    PersonDetectorOptions options;
};

PersonDetector::PersonDetector(PersonDetectorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

PersonDetector::~PersonDetector() = default;

bool PersonDetector::available() const
{
    return impl_ != nullptr && impl_->available;
}

std::vector<PersonDetection> PersonDetector::detect(const cv::Mat &bgr_frame)
{
    if (impl_ == nullptr)
    {
        return {};
    }
    return impl_->detect(bgr_frame);
}

void draw_person_detections(cv::Mat &frame, const std::vector<PersonDetection> &detections)
{
    for (const PersonDetection &det : detections)
    {
        cv::rectangle(frame,
                      cv::Point(det.x1, det.y1),
                      cv::Point(det.x2, det.y2),
                      cv::Scalar(0, 255, 0),
                      2);
    }
}

} // namespace serial_motor
