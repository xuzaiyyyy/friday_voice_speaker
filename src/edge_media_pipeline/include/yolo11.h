#ifndef _RKNN_DEMO_YOLO11_H_
#define _RKNN_DEMO_YOLO11_H_

#include "rknn_api.h"
#include <vector>

// 1. 补全缺失的结构体定义 (原本在 common.h 中)
typedef struct
{
    int left;
    int top;
    int right;
    int bottom;
} image_rect_t;

typedef struct
{
    int x_pad;
    int y_pad;
    float scale;
} letterbox_t;

typedef struct
{
    image_rect_t box;
    float prop;
    int cls_id;
} object_detect_result;

typedef struct
{
    int id;
    int count;
    object_detect_result results[128]; // OBJ_NUMB_MAX_SIZE
} object_detect_result_list;

// 2. rknn_app_context_t 定义
typedef struct
{
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    int model_channel;
    int model_width;
    int model_height;
    bool is_quant;
} rknn_app_context_t;

// 3. 函数声明
int post_process(rknn_app_context_t *app_ctx, void *outputs, letterbox_t *letter_box, float conf_threshold, float nms_threshold, object_detect_result_list *od_results);

#endif //_RKNN_DEMO_YOLO11_H_