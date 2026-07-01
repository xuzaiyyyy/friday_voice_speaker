#ifndef RKNN_INSTANCE_H
#define RKNN_INSTANCE_H

#include <string>
#include "rknn_api.h"
#include "yolo11.h"  // 依赖 yolo11.h 中的 rknn_app_context_t 定义

class RknnInstance
{
   public:
    // 将原有的上下文结构体作为成员变量暴露出来，方便外部的 inputs_set 和 post_process 调用
    rknn_app_context_t app_ctx;

    // 构造函数：初始化结构体
    RknnInstance();

    // 析构函数：生命周期结束时自动释放模型内存、属性数组和上下文句柄
    ~RknnInstance();

    // 初始化方法
    // @param model_path: RKNN 模型文件的物理路径
    // @return: 成功返回 0，失败返回 -1
    int Init(const std::string& model_path);
};

#endif  // RKNN_INSTANCE_H