#include "yolo11.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

// 硬编码标签，防止找不到文件报错
static const char* labels[] = {"person",        "bicycle",      "car",
                               "motorcycle",    "airplane",     "bus",
                               "train",         "truck",        "boat",
                               "traffic light", "fire hydrant", "stop sign",
                               "parking meter", "bench",        "bird",
                               "cat",           "dog",          "horse",
                               "sheep",         "cow",          "elephant",
                               "bear",          "zebra",        "giraffe",
                               "backpack",      "umbrella",     "handbag",
                               "tie",           "suitcase",     "frisbee",
                               "skis",          "snowboard",    "sports ball",
                               "kite",          "baseball bat", "baseball glove",
                               "skateboard",    "surfboard",    "tennis racket",
                               "bottle",        "wine glass",   "cup",
                               "fork",          "knife",        "spoon",
                               "bowl",          "banana",       "apple",
                               "sandwich",      "orange",       "broccoli",
                               "carrot",        "hot dog",      "pizza",
                               "donut",         "cake",         "chair",
                               "couch",         "potted plant", "bed",
                               "dining table",  "toilet",       "tv",
                               "laptop",        "mouse",        "remote",
                               "keyboard",      "cell phone",   "microwave",
                               "oven",          "toaster",      "sink",
                               "refrigerator",  "book",         "clock",
                               "vase",          "scissors",     "teddy bear",
                               "hair drier",    "toothbrush"};

#define OBJ_CLASS_NUM 80
#define PROP_BOX_SIZE (5 + OBJ_CLASS_NUM)
#define MAX_DETECT_RESULT_COUNT 128

inline static int clamp(float val, int min, int max)
{
    return val > min ? (val < max ? static_cast<int>(val) : max) : min;
}

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1,
                              float ymin1, float xmax1, float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0f);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0f);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) +
              (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int nms(int validCount, std::vector<float>& outputLocations, std::vector<int> classIds,
               std::vector<int>& order, int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        int n = order[i];
        if (n == -1 || classIds[n] != filterId)
            continue;
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
                continue;
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];
            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];
            float iou   = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);
            if (iou > threshold)
                order[j] = -1;
        }
    }
    return 0;
}

static int quick_sort_indice_inverse(std::vector<float>& input, int left, int right,
                                     std::vector<int>& indices)
{
    float key;
    int   key_index;
    int   low  = left;
    int   high = right;
    if (left < right)
    {
        key_index = indices[left];
        key       = input[left];
        while (low < high)
        {
            while (low < high && input[high] <= key) high--;
            input[low]   = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key) low++;
            input[high]   = input[low];
            indices[high] = indices[low];
        }
        input[low]   = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

static float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }
static float maybe_sigmoid(float x)
{
    // 已经是概率值时不再 sigmoid；logit 或反量化值超出 [0,1] 时才 sigmoid。
    if (x >= 0.0f && x <= 1.0f)
        return x;
    return sigmoid(x);
}

inline static int32_t __clip(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return static_cast<int32_t>(f);
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    return static_cast<int8_t>(__clip(dst_val, -128, 127));
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
{
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

static float deqnt_affine_u8_to_f32(uint8_t qnt, int32_t zp, float scale)
{
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

static float fp16_to_fp32(uint16_t h)
{
    const uint16_t h_exp = (h & 0x7C00u);
    const uint16_t h_sig = (h & 0x03FFu);
    const uint32_t sign  = static_cast<uint32_t>(h & 0x8000u) << 16;

    uint32_t f = 0;
    if (h_exp == 0)
    {
        if (h_sig == 0)
        {
            f = sign;
        }
        else
        {
            float mant = static_cast<float>(h_sig) / 1024.0f;
            float val  = std::ldexp(mant, -14);
            return sign ? -val : val;
        }
    }
    else if (h_exp == 0x7C00u)
    {
        f = sign | 0x7F800000u | (static_cast<uint32_t>(h_sig) << 13);
    }
    else
    {
        const uint32_t exp = static_cast<uint32_t>((h_exp >> 10) + (127 - 15));
        f                  = sign | (exp << 23) | (static_cast<uint32_t>(h_sig) << 13);
    }

    float out = 0.0f;
    std::memcpy(&out, &f, sizeof(out));
    return out;
}

static int64_t TensorElementCount(const rknn_tensor_attr& attr)
{
    int64_t count = 1;
    if (attr.n_dims == 0)
        return 0;
    for (uint32_t i = 0; i < attr.n_dims; ++i)
    {
        if (attr.dims[i] <= 0)
            return 0;
        count *= attr.dims[i];
    }
    return count;
}

static int TensorDim(const rknn_tensor_attr& attr, uint32_t index)
{
    return index < attr.n_dims ? attr.dims[index] : 0;
}

static float ReadTensorValue(const void* data, int64_t index, const rknn_tensor_attr& attr)
{
    if (!data || index < 0)
        return 0.0f;

    switch (attr.type)
    {
        case RKNN_TENSOR_FLOAT32:
            return static_cast<const float*>(data)[index];
        case RKNN_TENSOR_FLOAT16:
            return fp16_to_fp32(static_cast<const uint16_t*>(data)[index]);
        case RKNN_TENSOR_UINT8:
            return deqnt_affine_u8_to_f32(static_cast<const uint8_t*>(data)[index], attr.zp,
                                          attr.scale);
        case RKNN_TENSOR_INT8:
        default:
            return deqnt_affine_to_f32(static_cast<const int8_t*>(data)[index], attr.zp,
                                       attr.scale);
    }
}

static void AddYolo85Box(const std::vector<float>& v, int model_w, int model_h,
                         float conf_threshold, std::vector<float>& boxes,
                         std::vector<float>& objProbs, std::vector<int>& classId)
{
    if (v.size() < PROP_BOX_SIZE)
        return;

    float obj            = maybe_sigmoid(v[4]);
    int   best_cls       = -1;
    float best_cls_score = 0.0f;
    for (int c = 0; c < OBJ_CLASS_NUM; ++c)
    {
        const float cls_score = maybe_sigmoid(v[5 + c]);
        if (cls_score > best_cls_score)
        {
            best_cls_score = cls_score;
            best_cls       = c;
        }
    }

    const float score = obj * best_cls_score;
    if (score < conf_threshold || best_cls < 0)
        return;

    float cx = v[0];
    float cy = v[1];
    float w  = v[2];
    float h  = v[3];
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) || !std::isfinite(h) ||
        w <= 0.0f || h <= 0.0f)
    {
        return;
    }

    // 有些导出模型输出归一化坐标，有些输出输入尺度坐标。这里自动兼容。
    const float max_coord =
        std::max(std::max(std::fabs(cx), std::fabs(cy)), std::max(std::fabs(w), std::fabs(h)));
    if (max_coord <= 2.0f)
    {
        cx *= model_w;
        w *= model_w;
        cy *= model_h;
        h *= model_h;
    }

    boxes.push_back(cx - w * 0.5f);
    boxes.push_back(cy - h * 0.5f);
    boxes.push_back(w);
    boxes.push_back(h);
    objProbs.push_back(score);
    classId.push_back(best_cls);
}

static float ReadYoloOutputValue(const rknn_output& output, const rknn_tensor_attr& attr,
                                 int64_t index)
{
    if (!output.buf || index < 0)
        return 0.0f;

    // When rknn_output.want_float = 1, RKNN returns float buffers even though
    // the original tensor attr is INT8. This matches the standalone
    // person_detector.cpp that has already been verified on RK3566.
    if (output.want_float)
    {
        return static_cast<const float*>(output.buf)[index];
    }

    return ReadTensorValue(output.buf, index, attr);
}

static int process_yolo85_tensor(const rknn_output& output, const rknn_tensor_attr& attr,
                                 int model_w, int model_h, float conf_threshold,
                                 std::vector<float>& boxes, std::vector<float>& objProbs,
                                 std::vector<int>& classId)
{
    if (!output.buf || attr.n_dims != 4 || model_w <= 0 || model_h <= 0)
        return 0;

    // This branch intentionally follows the previously verified
    // person_detector.cpp postprocess for yolov6n_85.rknn:
    //   output dims: [1, 85, H, W], NCHW
    //   channels 0..3: l/t/r/b distances around grid center
    //   channel 4: object gate
    //   channels 5..84: class scores, COCO class 0 = person
    const int channels = TensorDim(attr, 1);
    const int grid_h   = TensorDim(attr, 2);
    const int grid_w   = TensorDim(attr, 3);
    if (channels < PROP_BOX_SIZE || grid_h <= 0 || grid_w <= 0)
        return 0;

    const int grid_len = grid_h * grid_w;
    int       stride   = 0;
    if (grid_w > 0)
        stride = model_w / grid_w;
    if (stride <= 0)
        stride = 8;

    int           validCount        = 0;
    constexpr int kPersonClassId    = 0;
    constexpr int kClassScoreOffset = 5;

    for (int y = 0; y < grid_h; ++y)
    {
        for (int x = 0; x < grid_w; ++x)
        {
            const int   offset      = y * grid_w + x;
            const float object_gate = ReadYoloOutputValue(output, attr, 4LL * grid_len + offset);
            if (object_gate < conf_threshold)
            {
                continue;
            }

            int   best_class_id   = 0;
            float best_class_conf = ReadYoloOutputValue(
                output, attr,
                static_cast<int64_t>(kClassScoreOffset + kPersonClassId) * grid_len + offset);
            for (int cls = 1; cls < OBJ_CLASS_NUM; ++cls)
            {
                const float class_conf = ReadYoloOutputValue(
                    output, attr,
                    static_cast<int64_t>(kClassScoreOffset + cls) * grid_len + offset);
                if (class_conf > best_class_conf)
                {
                    best_class_id   = cls;
                    best_class_conf = class_conf;
                }
            }

            // Keep the same behavior as the tested standalone detector:
            // only draw COCO class 0(person).
            if (best_class_id != kPersonClassId || best_class_conf < conf_threshold)
            {
                continue;
            }

            const float l = ReadYoloOutputValue(output, attr, 0LL * grid_len + offset);
            const float t = ReadYoloOutputValue(output, attr, 1LL * grid_len + offset);
            const float r = ReadYoloOutputValue(output, attr, 2LL * grid_len + offset);
            const float b = ReadYoloOutputValue(output, attr, 3LL * grid_len + offset);

            const float box_x1 = (x + 0.5f - l) * stride;
            const float box_y1 = (y + 0.5f - t) * stride;
            const float box_x2 = (x + 0.5f + r) * stride;
            const float box_y2 = (y + 0.5f + b) * stride;

            if (!std::isfinite(box_x1) || !std::isfinite(box_y1) || !std::isfinite(box_x2) ||
                !std::isfinite(box_y2) || box_x2 <= box_x1 || box_y2 <= box_y1)
            {
                continue;
            }

            boxes.push_back(box_x1);
            boxes.push_back(box_y1);
            boxes.push_back(box_x2 - box_x1);
            boxes.push_back(box_y2 - box_y1);
            objProbs.push_back(best_class_conf);
            classId.push_back(best_class_id);
            ++validCount;
        }
    }

    return validCount;
}

static int fill_detect_results(const std::vector<float>& filterBoxes,
                               const std::vector<float>& objProbs, const std::vector<int>& classId,
                               float nms_threshold, letterbox_t* letter_box,
                               object_detect_result_list* od_results)
{
    const int validCount = static_cast<int>(objProbs.size());
    if (validCount <= 0)
        return 0;

    std::vector<int> indexArray;
    indexArray.reserve(validCount);
    for (int i = 0; i < validCount; ++i) indexArray.push_back(i);

    std::vector<float> sortedScores = objProbs;
    quick_sort_indice_inverse(sortedScores, 0, validCount - 1, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));
    for (auto c : class_set)
    {
        nms(validCount, const_cast<std::vector<float>&>(filterBoxes), classId, indexArray, c,
            nms_threshold);
    }

    int last_count    = 0;
    od_results->count = 0;
    const float scale = (letter_box && letter_box->scale > 0.0f) ? letter_box->scale : 1.0f;
    const float x_pad = letter_box ? letter_box->x_pad : 0.0f;
    const float y_pad = letter_box ? letter_box->y_pad : 0.0f;

    for (int i = 0; i < validCount && last_count < MAX_DETECT_RESULT_COUNT; ++i)
    {
        if (indexArray[i] == -1)
            continue;
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0] - x_pad;
        float y1 = filterBoxes[n * 4 + 1] - y_pad;
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];

        od_results->results[last_count].box.left   = static_cast<int>(x1 / scale);
        od_results->results[last_count].box.top    = static_cast<int>(y1 / scale);
        od_results->results[last_count].box.right  = static_cast<int>(x2 / scale);
        od_results->results[last_count].box.bottom = static_cast<int>(y2 / scale);
        od_results->results[last_count].prop       = objProbs[n];
        od_results->results[last_count].cls_id     = classId[n];
        last_count++;
    }

    od_results->count = last_count;
    return 0;
}

static void compute_dfl(float* tensor, int dfl_len, float* box)
{
    for (int b = 0; b < 4; b++)
    {
        float exp_t[32];
        float exp_sum = 0;
        float acc_sum = 0;
        if (dfl_len <= 0 || dfl_len > 32)
        {
            box[b] = 0.0f;
            continue;
        }
        for (int i = 0; i < dfl_len; i++)
        {
            exp_t[i] = expf(tensor[i + b * dfl_len]);
            exp_sum += exp_t[i];
        }
        if (exp_sum <= 0.0f)
        {
            box[b] = 0.0f;
            continue;
        }
        for (int i = 0; i < dfl_len; i++)
        {
            acc_sum += exp_t[i] / exp_sum * i;
        }
        box[b] = acc_sum;
    }
}

static int process_i8(int8_t* box_tensor, int32_t box_zp, float box_scale, int8_t* score_tensor,
                      int32_t score_zp, float score_scale, int8_t* score_sum_tensor,
                      int32_t score_sum_zp, float score_sum_scale, int grid_h, int grid_w,
                      int stride, int dfl_len, std::vector<float>& boxes,
                      std::vector<float>& objProbs, std::vector<int>& classId, float threshold)
{
    int    validCount         = 0;
    int    grid_len           = grid_h * grid_w;
    int8_t score_thres_i8     = qnt_f32_to_affine(threshold, score_zp, score_scale);
    int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset       = i * grid_w + j;
            int max_class_id = -1;

            if (score_sum_tensor != nullptr)
            {
                if (score_sum_tensor[offset] < score_sum_thres_i8)
                    continue;
            }

            int8_t max_score = -score_zp;
            for (int c = 0; c < OBJ_CLASS_NUM; c++)
            {
                if ((score_tensor[offset] > score_thres_i8) && (score_tensor[offset] > max_score))
                {
                    max_score    = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            if (max_score > score_thres_i8)
            {
                offset = i * grid_w + j;
                float box[4];
                float before_dfl[32 * 4];
                if (dfl_len <= 0 || dfl_len > 32)
                    continue;
                for (int k = 0; k < dfl_len * 4; k++)
                {
                    before_dfl[k] = deqnt_affine_to_f32(box_tensor[offset], box_zp, box_scale);
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1 = (-box[0] + j + 0.5f) * stride;
                float y1 = (-box[1] + i + 0.5f) * stride;
                float x2 = (box[2] + j + 0.5f) * stride;
                float y2 = (box[3] + i + 0.5f) * stride;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(x2 - x1);
                boxes.push_back(y2 - y1);
                objProbs.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount++;
            }
        }
    }
    return validCount;
}

static bool IsDflLayout(rknn_app_context_t* app_ctx)
{
    if (!app_ctx || !app_ctx->output_attrs)
        return false;
    if (!(app_ctx->io_num.n_output == 6 || app_ctx->io_num.n_output == 9))
        return false;
    const int output_per_branch = app_ctx->io_num.n_output / 3;
    if (output_per_branch < 2)
        return false;
    if (app_ctx->output_attrs[0].n_dims < 4)
        return false;
    const int dfl_len = app_ctx->output_attrs[0].dims[1] / 4;
    return dfl_len > 0 && dfl_len <= 32;
}

static int post_process_dfl_i8(rknn_app_context_t* app_ctx, rknn_output* _outputs,
                               letterbox_t* letter_box, float conf_threshold, float nms_threshold,
                               object_detect_result_list* od_results)
{
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int>   classId;
    int                validCount        = 0;
    const int          model_in_h        = app_ctx->model_height;
    const int          dfl_len           = app_ctx->output_attrs[0].dims[1] / 4;
    const int          output_per_branch = app_ctx->io_num.n_output / 3;

    for (int i = 0; i < 3; i++)
    {
        void*   score_sum       = nullptr;
        int32_t score_sum_zp    = 0;
        float   score_sum_scale = 1.0f;
        if (output_per_branch == 3)
        {
            score_sum       = _outputs[i * output_per_branch + 2].buf;
            score_sum_zp    = app_ctx->output_attrs[i * output_per_branch + 2].zp;
            score_sum_scale = app_ctx->output_attrs[i * output_per_branch + 2].scale;
        }

        const int box_idx   = i * output_per_branch;
        const int score_idx = i * output_per_branch + 1;
        if (score_idx >= static_cast<int>(app_ctx->io_num.n_output))
            continue;

        const int grid_h = app_ctx->output_attrs[box_idx].dims[2];
        const int grid_w = app_ctx->output_attrs[box_idx].dims[3];
        if (grid_h <= 0 || grid_w <= 0)
            continue;
        const int stride = model_in_h / grid_h;

        validCount += process_i8(
            static_cast<int8_t*>(_outputs[box_idx].buf), app_ctx->output_attrs[box_idx].zp,
            app_ctx->output_attrs[box_idx].scale, static_cast<int8_t*>(_outputs[score_idx].buf),
            app_ctx->output_attrs[score_idx].zp, app_ctx->output_attrs[score_idx].scale,
            static_cast<int8_t*>(score_sum), score_sum_zp, score_sum_scale, grid_h, grid_w, stride,
            dfl_len, filterBoxes, objProbs, classId, conf_threshold);
    }

    (void)validCount;
    return fill_detect_results(filterBoxes, objProbs, classId, nms_threshold, letter_box,
                               od_results);
}

// 主入口函数
int post_process(rknn_app_context_t* app_ctx, void* outputs, letterbox_t* letter_box,
                 float conf_threshold, float nms_threshold, object_detect_result_list* od_results)
{
    if (!od_results)
        return -1;
    std::memset(od_results, 0, sizeof(object_detect_result_list));

    if (!app_ctx || !outputs || !app_ctx->output_attrs || app_ctx->io_num.n_output == 0)
    {
        return -1;
    }

    rknn_output* _outputs = static_cast<rknn_output*>(outputs);

    if (IsDflLayout(app_ctx))
    {
        return post_process_dfl_i8(app_ctx, _outputs, letter_box, conf_threshold, nms_threshold,
                                   od_results);
    }

    // yolov6n_85.rknn 为 3 路 YOLO85 风格输出，使用与单独测试程序一致的解析方式。
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int>   classId;

    for (uint32_t i = 0; i < app_ctx->io_num.n_output; ++i)
    {
        process_yolo85_tensor(_outputs[i], app_ctx->output_attrs[i], app_ctx->model_width,
                              app_ctx->model_height, conf_threshold, filterBoxes, objProbs,
                              classId);
    }

    return fill_detect_results(filterBoxes, objProbs, classId, nms_threshold, letter_box,
                               od_results);
}

int   init_post_process() { return 0; }
void  deinit_post_process() {}
char* coco_cls_to_name(int cls_id)
{
    if (cls_id < 0 || cls_id >= OBJ_CLASS_NUM)
        return (char*)"null";
    return (char*)labels[cls_id];
}
