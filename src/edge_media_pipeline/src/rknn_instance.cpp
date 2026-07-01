#include "rknn_instance.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sstream>

namespace
{
const char* TensorFmtName(rknn_tensor_format fmt)
{
    switch (fmt)
    {
        case RKNN_TENSOR_NCHW:
            return "NCHW";
        case RKNN_TENSOR_NHWC:
            return "NHWC";
        default:
            return "UNKNOWN";
    }
}

const char* TensorTypeName(rknn_tensor_type type)
{
    switch (type)
    {
        case RKNN_TENSOR_FLOAT32:
            return "FLOAT32";
        case RKNN_TENSOR_FLOAT16:
            return "FLOAT16";
        case RKNN_TENSOR_INT8:
            return "INT8";
        case RKNN_TENSOR_UINT8:
            return "UINT8";
        case RKNN_TENSOR_INT32:
            return "INT32";
        default:
            return "UNKNOWN";
    }
}

const char* TensorQntTypeName(rknn_tensor_qnt_type qnt_type)
{
    switch (qnt_type)
    {
        case RKNN_TENSOR_QNT_NONE:
            return "NONE";
        case RKNN_TENSOR_QNT_DFP:
            return "DFP";
        case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC:
            return "AFFINE_ASYMMETRIC";
        default:
            return "UNKNOWN";
    }
}

std::string TensorDimsToString(const rknn_tensor_attr& attr)
{
    std::ostringstream oss;
    oss << "[";
    for (uint32_t i = 0; i < attr.n_dims; ++i)
    {
        if (i > 0)
        {
            oss << ",";
        }
        oss << attr.dims[i];
    }
    oss << "]";
    return oss.str();
}

bool ResolveInputShape(const rknn_tensor_attr& attr, int* width, int* height, int* channels)
{
    if (!width || !height || !channels || attr.n_dims < 3)
    {
        return false;
    }

    int w = 0;
    int h = 0;
    int c = 0;

    if (attr.n_dims == 4)
    {
        if (attr.fmt == RKNN_TENSOR_NCHW)
        {
            c = attr.dims[1];
            h = attr.dims[2];
            w = attr.dims[3];
        }
        else if (attr.fmt == RKNN_TENSOR_NHWC)
        {
            h = attr.dims[1];
            w = attr.dims[2];
            c = attr.dims[3];
        }
        else if (attr.dims[1] == 1 || attr.dims[1] == 3 || attr.dims[1] == 4)
        {
            c = attr.dims[1];
            h = attr.dims[2];
            w = attr.dims[3];
        }
        else
        {
            h = attr.dims[1];
            w = attr.dims[2];
            c = attr.dims[3];
        }
    }
    else if (attr.n_dims == 3)
    {
        if (attr.fmt == RKNN_TENSOR_NCHW || attr.dims[0] == 1 || attr.dims[0] == 3 ||
            attr.dims[0] == 4)
        {
            c = attr.dims[0];
            h = attr.dims[1];
            w = attr.dims[2];
        }
        else
        {
            h = attr.dims[0];
            w = attr.dims[1];
            c = attr.dims[2];
        }
    }

    if (c <= 0)
    {
        c = 3;
    }
    if (w <= 0 || h <= 0)
    {
        return false;
    }

    const int64_t tensor_bytes   = static_cast<int64_t>(attr.size);
    const int64_t resolved_bytes = static_cast<int64_t>(w) * h * c;
    if (tensor_bytes > 0 && resolved_bytes != tensor_bytes && c > 0 && tensor_bytes % c == 0)
    {
        const int64_t area = tensor_bytes / c;
        if (w > 0 && area % w == 0)
        {
            h = static_cast<int>(area / w);
        }
        else if (h > 0 && area % h == 0)
        {
            w = static_cast<int>(area / h);
        }
    }

    *width    = w;
    *height   = h;
    *channels = c;
    return true;
}
}  // namespace

// 内部辅助函数：读取模型文件到内存
static unsigned char* load_model(const char* filename, int* model_size)
{
    FILE* fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char* data = (unsigned char*)malloc(size);
    if (data == NULL)
    {
        fclose(fp);
        return NULL;
    }
    fread(data, 1, size, fp);
    fclose(fp);
    *model_size = size;
    return data;
}

RknnInstance::RknnInstance()
{
    // 构造时将上下文彻底清零，防止野指针
    memset(&app_ctx, 0, sizeof(rknn_app_context_t));
}

RknnInstance::~RknnInstance()
{
    // 析构时自动清理所有资源，无需外部手动调用 free
    if (app_ctx.input_attrs)
    {
        free(app_ctx.input_attrs);
        app_ctx.input_attrs = nullptr;
    }
    if (app_ctx.output_attrs)
    {
        free(app_ctx.output_attrs);
        app_ctx.output_attrs = nullptr;
    }
    if (app_ctx.rknn_ctx != 0)
    {
        rknn_destroy(app_ctx.rknn_ctx);
        app_ctx.rknn_ctx = 0;
    }
    std::cout << "[RKNN] Instance destroyed. Resources released cleanly." << std::endl;
}

int RknnInstance::Init(const std::string& model_path)
{
    int model_len = 0;

    // 1. 加载模型到内存
    unsigned char* model_data = load_model(model_path.c_str(), &model_len);
    if (!model_data)
    {
        std::cerr << "[RKNN] Failed to load RKNN model from " << model_path << std::endl;
        return -1;
    }

    // 2. 初始化 RKNN 引擎
    int ret = rknn_init(&app_ctx.rknn_ctx, model_data, model_len, 0, NULL);

    // 【关键优化】：初始化完成后，模型已被 NPU 驱动接管，应用层的 model_data
    // 可以立刻释放，节省几十MB内存！
    free(model_data);

    if (ret < 0)
    {
        std::cerr << "[RKNN] rknn_init failed! error code: " << ret << std::endl;
        return -1;
    }

    // 3. 查询输入输出张量数量
    rknn_query(app_ctx.rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &app_ctx.io_num, sizeof(app_ctx.io_num));

    // 4. 分配并查询输入输出属性内存
    app_ctx.input_attrs =
        (rknn_tensor_attr*)malloc(app_ctx.io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx.output_attrs =
        (rknn_tensor_attr*)malloc(app_ctx.io_num.n_output * sizeof(rknn_tensor_attr));

    for (uint32_t i = 0; i < app_ctx.io_num.n_input; i++)
    {
        app_ctx.input_attrs[i].index = i;
        rknn_query(app_ctx.rknn_ctx, RKNN_QUERY_INPUT_ATTR, &(app_ctx.input_attrs[i]),
                   sizeof(rknn_tensor_attr));
    }
    for (uint32_t i = 0; i < app_ctx.io_num.n_output; i++)
    {
        app_ctx.output_attrs[i].index = i;
        rknn_query(app_ctx.rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &(app_ctx.output_attrs[i]),
                   sizeof(rknn_tensor_attr));
    }

    // 5. 从 RKNN 模型输入张量读取真实尺寸。不要写死 640x640；RK3566 上当前
    // yolov6n_85.rknn 的输入是 640x352x3，写死会导致 rknn_inputs_set 崩链路。
    int model_width   = 0;
    int model_height  = 0;
    int model_channel = 0;
    if (!ResolveInputShape(app_ctx.input_attrs[0], &model_width, &model_height, &model_channel))
    {
        std::cerr << "[RKNN] Failed to resolve input shape from attr dims="
                  << TensorDimsToString(app_ctx.input_attrs[0])
                  << ", fmt=" << TensorFmtName(app_ctx.input_attrs[0].fmt)
                  << ", size=" << app_ctx.input_attrs[0].size << std::endl;
        return -1;
    }

    app_ctx.is_quant = false;
    for (uint32_t i = 0; i < app_ctx.io_num.n_output; ++i)
    {
        if (app_ctx.output_attrs[i].qnt_type != RKNN_TENSOR_QNT_NONE)
        {
            app_ctx.is_quant = true;
            break;
        }
    }
    app_ctx.model_width   = model_width;
    app_ctx.model_height  = model_height;
    app_ctx.model_channel = model_channel;

    std::cout << "[RKNN] io_num: input=" << app_ctx.io_num.n_input
              << ", output=" << app_ctx.io_num.n_output << std::endl;

    std::cout << "[RKNN] input[0] dims=" << TensorDimsToString(app_ctx.input_attrs[0])
              << ", fmt=" << TensorFmtName(app_ctx.input_attrs[0].fmt)
              << ", type=" << TensorTypeName(app_ctx.input_attrs[0].type)
              << ", qnt=" << TensorQntTypeName(app_ctx.input_attrs[0].qnt_type)
              << ", zp=" << app_ctx.input_attrs[0].zp << ", scale=" << app_ctx.input_attrs[0].scale
              << ", size=" << app_ctx.input_attrs[0].size
              << ", size_with_stride=" << app_ctx.input_attrs[0].size_with_stride
              << ", resolved=" << app_ctx.model_width << "x" << app_ctx.model_height << "x"
              << app_ctx.model_channel << std::endl;

    for (uint32_t i = 0; i < app_ctx.io_num.n_output; ++i)
    {
        std::cout << "[RKNN] output[" << i
                  << "] dims=" << TensorDimsToString(app_ctx.output_attrs[i])
                  << ", fmt=" << TensorFmtName(app_ctx.output_attrs[i].fmt)
                  << ", type=" << TensorTypeName(app_ctx.output_attrs[i].type)
                  << ", qnt=" << TensorQntTypeName(app_ctx.output_attrs[i].qnt_type)
                  << ", zp=" << app_ctx.output_attrs[i].zp
                  << ", scale=" << app_ctx.output_attrs[i].scale
                  << ", size=" << app_ctx.output_attrs[i].size
                  << ", size_with_stride=" << app_ctx.output_attrs[i].size_with_stride << std::endl;
    }

    std::cout << "[RKNN] Model initialized successfully: " << model_path << std::endl;
    return 0;
}
