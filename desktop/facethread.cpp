#include "facethread.h"
#include "apppaths.h"
#include <fstream>
#include <cmath>
#include <QPainter> // 用于画中文
#include <QFont>
#include <QDir>
#include <QFileInfo>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdio.h>
#ifdef __linux__
#include <dlfcn.h>
#endif

using namespace cv;
using namespace std;

namespace
{
std::string modelPath(const QString &name)
{
    return AppPaths::existingProjectOrDesktopFile(QStringLiteral("models/") + name).toStdString();
}

std::string faceDatabasePath()
{
    return AppPaths::desktopFile(QStringLiteral("face_database.dat")).toStdString();
}

QString loadedRknnRuntimePath()
{
#ifdef __linux__
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(reinterpret_cast<void*>(rknn_init), &info) && info.dli_fname) {
        return QString::fromLocal8Bit(info.dli_fname);
    }
#endif
    return QStringLiteral("unknown");
}
} // namespace

// ================= 配置参数 =================

// RetinaFace 参数 (OrangePi NPU 通常使用 320x320)
const int DET_INPUT_W = 320;
const int DET_INPUT_H = 320;
const float CONF_THRESH = 0.5f;
const float NMS_THRESH  = 0.4f;

// MobileFaceNet 参数
const int REC_INPUT_W = 112;
const int REC_INPUT_H = 112;
const int FEAT_DIM    = 128;
// 识别阈值：建议在 0.45 - 0.55 之间微调
const float SIM_THRESH = 0.88f;

// Anchor 配置
const std::vector<int> STRIDES = {8, 16, 32};
const std::vector<std::vector<int>> MIN_SIZES = {{16, 32}, {64, 128}, {256, 512}};
const float VARIANCES[2] = {0.1f, 0.2f};
const float REF_POINTS[5][2] = {
    {30.2946f, 51.6963f}, {65.5318f, 51.6963f}, {48.0252f, 71.7366f},
    {33.5493f, 92.3655f}, {62.7299f, 92.3655f}
};

// 辅助函数：读取二进制模型文件
static unsigned char *load_model_file(const char *filename, int *model_size) {
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) return NULL;
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *data = (unsigned char *)malloc(size);
    fread(data, 1, size, fp);
    fclose(fp);
    *model_size = size;
    return data;
}

FaceThread::FaceThread(QObject *parent) : QThread(parent)
{
    m_stop = false;
    m_request_register = false;
    m_request_delete = false;
    m_is_recognizing = false;
    
    // 初始化句柄为空
    ctx_det = 0;
    ctx_rec = 0;
    
    // 初始化录入计数器
    m_register_frame_count = 0;
}

FaceThread::~FaceThread() {
    stop();
    wait();
    releaseRKNN(); // 释放 NPU 资源
}

void FaceThread::stop() { m_stop = true; }

// 控制接口实现
void FaceThread::registerFace(QString name) {
    m_target_name = name;
    m_request_register = true;
    
    // 【新增】重置录入状态
    m_register_frame_count = 0;
    m_register_feat_sum.assign(FEAT_DIM, 0.0f);
}

void FaceThread::deleteRecord(QString name) {
    m_target_name = name;
    m_request_delete = true;
}

void FaceThread::startRecognition() {
    m_is_recognizing = true;
    emit logMessage(">> 识别模式已开启");
}

void FaceThread::stopRecognition() {
    m_is_recognizing = false;
    emit logMessage(">> 识别已停止，仅检测");
}

// 核心运行循环
void FaceThread::run()
{
    try {
    emit logMessage(QStringLiteral("RKNN runtime: ") + loadedRknnRuntimePath());

    // 1. 初始化 NPU 模型
    const std::string detPath = modelPath(QStringLiteral("RetinaFace.rknn"));
    const std::string recPath = modelPath(QStringLiteral("rec.rknn"));
    int ret = initRKNN(detPath.c_str(), &ctx_det);
    if (ret < 0) {
        emit logMessage(QString("严重错误: RetinaFace 模型加载失败 ret=%1 err=%2 ")
                        .arg(ret)
                        .arg(m_lastRknnError.isEmpty() ? QStringLiteral("-") : m_lastRknnError)
                        + QString::fromStdString(detPath));
        return;
    }
    ret = initRKNN(recPath.c_str(), &ctx_rec);
    if (ret < 0) {
        ctx_rec = 0;
        emit logMessage(QString("警告: MobileFaceNet 模型加载失败，猫眼将只做人脸检测 ret=%1 err=%2 ")
                        .arg(ret)
                        .arg(m_lastRknnError.isEmpty() ? QStringLiteral("-") : m_lastRknnError)
                        + QString::fromStdString(recPath));
    }

    loadFaceData(); // 加载用户数据
    priors = generatePriors(); // 生成 Anchor

    // 2. 打开摄像头
    VideoCapture cap(0);
    if (!cap.isOpened()) {
        cap.open(1); // 尝试索引 1
        if (!cap.isOpened()) {
            emit logMessage("Error: 摄像头无法打开");
            return;
        }
    }
    
    // 设置分辨率
    cap.set(CAP_PROP_FRAME_WIDTH, 640);
    cap.set(CAP_PROP_FRAME_HEIGHT, 480);

    Mat frame, img_det;
    emit logMessage(QString("NPU 系统就绪，已加载 %1 个用户").arg(m_userDatabase.size()));

    // 用于 GUI 绘制的结构
    struct DrawInfo {
        Rect box;
        QString text;
        Scalar color;
    };

    while (!m_stop) {
        cap >> frame;
        if (frame.empty()) break;

        // --- 1. 处理删除请求 ---
        if (m_request_delete) {
            bool found = false;
            for (auto it = m_userDatabase.begin(); it != m_userDatabase.end(); ) {
                if (it->name == m_target_name) {
                    it = m_userDatabase.erase(it);
                    found = true;
                } else {
                    ++it;
                }
            }
            if (found) {
                saveFaceData();
                emit logMessage("成功删除: " + m_target_name);
            } else {
                emit logMessage("未找到: " + m_target_name);
            }
            m_request_delete = false;
        }

        Mat raw_frame = frame.clone();
        
        // 缩放并转 RGB (RKNN 输入通常是 RGB 格式)
        resize(frame, img_det, Size(DET_INPUT_W, DET_INPUT_H));
        cvtColor(img_det, img_det, COLOR_BGR2RGB);

        // --- 2. NPU 人脸检测 ---
        vector<FaceObject> faces = detectFacesNPU(img_det);
        std::vector<DrawInfo> drawList;

        for (auto &face : faces) {
            // 坐标还原
            float sx = (float)frame.cols / DET_INPUT_W;
            float sy = (float)frame.rows / DET_INPUT_H;
            Rect box(face.box.x * sx, face.box.y * sy, face.box.width * sx, face.box.height * sy);
            box = box & Rect(0, 0, frame.cols, frame.rows); // 边界保护

            // =============================================
            // 【优化方案三】质量门控：人脸过小则忽略
            // 防止误识别人脸（例如远处的人）
            // =============================================
            if (box.width < 70 || box.height < 70) {
                // 画个灰色框提示
                DrawInfo info; 
                info.box = box; 
                info.text = "请靠近"; 
                info.color = Scalar(128, 128, 128); // 灰
                drawList.push_back(info);
                
                // 用 OpenCV 画灰框
                rectangle(frame, box, Scalar(128, 128, 128), 1);
                continue; // 跳过后续昂贵的 NPU 特征提取
            }

            vector<Point2f> lms;
            for(auto &p : face.landmarks) lms.push_back(Point2f(p.x * sx, p.y * sy));

            QString labelText = "";
            Scalar boxColor = Scalar(255, 0, 0);

            // A. 普通检测模式
            if (!m_is_recognizing && !m_request_register) {
                boxColor = Scalar(255, 0, 0); // 蓝
                labelText = "等待检测";
            }
            // B. 处理录入或识别 (置信度过滤)
            else if (face.score > 0.85) { // 提高一点置信度门槛
                if (!ctx_rec) {
                    boxColor = Scalar(0, 255, 255);
                    labelText = "识别模型不可用";
                    rectangle(frame, box, boxColor, 2);
                    DrawInfo info; info.box = box; info.text = labelText; info.color = boxColor;
                    drawList.push_back(info);
                    continue;
                }

                // 人脸对齐
                Mat aligned = alignFace(raw_frame, lms);
                // 转 RGB 喂给识别模型
                cvtColor(aligned, aligned, COLOR_BGR2RGB); 
                
                // --- NPU 特征提取 ---
                vector<float> feat = extractFeatureNPU(aligned);

                // =============================================
                // 【优化方案一】多帧平均录入逻辑
                // =============================================
                if (m_request_register) {
                    // 1. 累加特征
                    for(int i=0; i<FEAT_DIM; i++) {
                        m_register_feat_sum[i] += feat[i];
                    }
                    m_register_frame_count++;

                    // 2. 判断进度
                    if (m_register_frame_count < 5) {
                        boxColor = Scalar(255, 0, 255); // 紫
                        labelText = QString("录入中 %1/5").arg(m_register_frame_count);
                    } else {
                        // 3. 求平均并归一化
                        for(int i=0; i<FEAT_DIM; i++) {
                            m_register_feat_sum[i] /= 5.0f;
                        }
                        normalizeFeature(m_register_feat_sum); // 必须再次归一化

                        // 4. 保存
                        UserData newUser;
                        newUser.name = m_target_name;
                        newUser.feature = m_register_feat_sum;
                        m_userDatabase.push_back(newUser);
                        
                        saveFaceData();
                        emit logMessage("录入完成(优化版): " + m_target_name);
                        
                        // 重置
                        m_request_register = false;
                        boxColor = Scalar(0, 255, 0);
                        labelText = "录入成功";
                    }
                    
                    rectangle(frame, box, boxColor, 2);
                    DrawInfo info; info.box = box; info.text = labelText; info.color = boxColor;
                    drawList.push_back(info);
                    continue; // 录入时不识别
                }

                // --- 识别逻辑 ---
                if (m_is_recognizing) {
                    float max_sim = 0.0f;
                    QString matched_name = "Unknown";

                    for (const auto& user : m_userDatabase) {
                        float sim = calcSimilarity(feat, user.feature);
                        if (sim > max_sim) {
                            max_sim = sim;
                            if (max_sim > SIM_THRESH) matched_name = user.name;
                        }
                    }

                    if (max_sim > SIM_THRESH) {
                        boxColor = Scalar(0, 255, 0); // 绿
                        labelText = matched_name + QString(" (%1)").arg(max_sim, 0, 'f', 2) + " 请进";
                    } else {
                        boxColor = Scalar(0, 255, 255); // 黄
                        labelText = "陌生人";
                    }
                }
            }
            
            // OpenCV 画框
            rectangle(frame, box, boxColor, 2);
            
            // 记录信息供 Qt 画文字
            DrawInfo info; info.box = box; info.text = labelText; info.color = boxColor;
            drawList.push_back(info);
        }

        // --- 3. 绘制中文 (OpenCV -> Qt QPainter) ---
        cvtColor(frame, frame, COLOR_BGR2RGB);
        QImage qt_img((const unsigned char*)frame.data, frame.cols, frame.rows, frame.step, QImage::Format_RGB888);
        QImage finalImage = qt_img.copy();

        QPainter painter(&finalImage);
        QFont font = painter.font();
        font.setPixelSize(20);
        font.setBold(true);
        painter.setFont(font);

        for (const auto& item : drawList) {
            QColor txtColor = Qt::red;
            // 简单的颜色映射
            if (item.color[1] == 255 && item.color[2] == 0) txtColor = Qt::green;
            else if (item.color[0] == 255 && item.color[2] == 255) txtColor = Qt::magenta;
            else if (item.color[0] == 255 && item.color[2] == 0) txtColor = Qt::blue;
            else if (item.color[1] == 255 && item.color[2] == 255) txtColor = Qt::yellow;
            else if (item.color[0] == 128) txtColor = Qt::gray;

            painter.setPen(txtColor);
            painter.drawText(item.box.x, item.box.y - 10, item.text);
        }
        painter.end();

        emit frameProcessed(finalImage);
        QThread::msleep(10); 
    }
    cap.release();
    } catch (const std::exception &e) {
        emit logMessage(QStringLiteral("严重错误: 智能猫眼线程异常: ") + QString::fromLocal8Bit(e.what()));
    } catch (...) {
        emit logMessage(QStringLiteral("严重错误: 智能猫眼线程发生未知异常"));
    }
}

// ================= NPU 相关实现 =================

int FaceThread::initRKNN(const char* path, rknn_context* ctx) {
    m_lastRknnError.clear();
    int size = 0;
    unsigned char* model_data = load_model_file(path, &size);
    if (!model_data) {
        m_lastRknnError = QStringLiteral("cannot open model file");
        return -1001;
    }
    
    // 初始化 RKNN
    int ret = 0;
    try {
        ret = rknn_init(ctx, model_data, size, 0, NULL);
    } catch (const std::exception &e) {
        m_lastRknnError = QStringLiteral("rknn_init exception: ") + QString::fromLocal8Bit(e.what());
        free(model_data);
        return -1002;
    } catch (...) {
        m_lastRknnError = QStringLiteral("rknn_init unknown exception");
        free(model_data);
        return -1003;
    }
    free(model_data);
    
    if (ret < 0) {
        m_lastRknnError = QStringLiteral("rknn_init returned error");
        return ret;
    }
    return 0;
}

void FaceThread::releaseRKNN() {
    if (ctx_det) {
        rknn_destroy(ctx_det);
        ctx_det = 0;
    }
    if (ctx_rec) {
        rknn_destroy(ctx_rec);
        ctx_rec = 0;
    }
}

std::vector<FaceObject> FaceThread::detectFacesNPU(cv::Mat &img) {
    // 1. 设置输入
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = DET_INPUT_W * DET_INPUT_H * 3;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].pass_through = 0; // 0 表示让驱动处理归一化(如果模型带了mean/std)
    inputs[0].buf = img.data;
    
    rknn_inputs_set(ctx_det, 1, inputs);

    // 2. 推理
    rknn_run(ctx_det, NULL);

    // 3. 获取输出
    rknn_output outputs[3];
    memset(outputs, 0, sizeof(outputs));
    for(int i=0; i<3; i++) outputs[i].want_float = 1; // 强制转 float
    rknn_outputs_get(ctx_det, 3, outputs, NULL);

    // 4. 解码输出 (RetinaFace 3个分支)
    float *loc = NULL, *conf = NULL, *land = NULL;
    int total_anchors = priors.size(); 

    // 自动匹配输出层
    for(int i=0; i<3; i++) {
        int cnt = outputs[i].size / 4; // float 是 4 字节
        if (cnt == total_anchors * 4) loc = (float*)outputs[i].buf;
        else if (cnt == total_anchors * 2) conf = (float*)outputs[i].buf;
        else if (cnt == total_anchors * 10) land = (float*)outputs[i].buf;
    }

    vector<FaceObject> faces;
    if (loc && conf && land) {
        vector<Rect> boxes;
        vector<float> scores;
        vector<vector<Point2f>> landmarks_list;

        for (size_t i = 0; i < priors.size(); i++) {
            float score = conf[i * 2 + 1]; // 人脸类的置信度
            if (score > CONF_THRESH) {
                Vec4f prior = priors[i];
                float cx = prior[0] + loc[i*4+0] * VARIANCES[0] * prior[2];
                float cy = prior[1] + loc[i*4+1] * VARIANCES[0] * prior[3];
                float w  = prior[2] * exp(loc[i*4+2] * VARIANCES[1]);
                float h  = prior[3] * exp(loc[i*4+3] * VARIANCES[1]);
                
                vector<Point2f> lms;
                for(int j=0; j<5; j++) {
                    float lx = prior[0] + land[i*10+j*2+0] * VARIANCES[0] * prior[2];
                    float ly = prior[1] + land[i*10+j*2+1] * VARIANCES[0] * prior[3];
                    lms.push_back(Point2f(lx * DET_INPUT_W, ly * DET_INPUT_H));
                }
                boxes.push_back(Rect((cx-w/2)*DET_INPUT_W, (cy-h/2)*DET_INPUT_H, w*DET_INPUT_W, h*DET_INPUT_H));
                scores.push_back(score);
                landmarks_list.push_back(lms);
            }
        }
        
        // NMS 去重
        vector<int> indices;
        cv::dnn::NMSBoxes(boxes, scores, CONF_THRESH, NMS_THRESH, indices);
        for(int idx : indices) {
            FaceObject obj;
            obj.box = boxes[idx];
            obj.score = scores[idx];
            obj.landmarks = landmarks_list[idx];
            faces.push_back(obj);
        }
    }
    // 释放输出内存
    rknn_outputs_release(ctx_det, 3, outputs);
    return faces;
}

std::vector<float> FaceThread::extractFeatureNPU(cv::Mat &aligned) {
    // 1. 设置输入
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = REC_INPUT_W * REC_INPUT_H * 3;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].buf = aligned.data;
    
    rknn_inputs_set(ctx_rec, 1, inputs);

    // 2. 推理
    rknn_run(ctx_rec, NULL);

    // 3. 获取输出
    rknn_output outputs[1];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1;
    rknn_outputs_get(ctx_rec, 1, outputs, NULL);

    vector<float> feat;
    float* out_data = (float*)outputs[0].buf;
    for(int i=0; i<FEAT_DIM; i++) {
        feat.push_back(out_data[i]);
    }

    normalizeFeature(feat);
    rknn_outputs_release(ctx_rec, 1, outputs);
    return feat;
}

// ================= 标准算法与数据 IO =================

void FaceThread::saveFaceData() {
    const std::string dbPath = faceDatabasePath();
    const QFileInfo dbInfo(QString::fromStdString(dbPath));
    QDir().mkpath(dbInfo.absolutePath());

    ofstream out(dbPath, ios::binary | ios::trunc);
    if (!out.is_open()) {
        emit logMessage(QStringLiteral("警告: 人脸数据库无法写入: ") + QString::fromStdString(dbPath));
        return;
    }

    int count = m_userDatabase.size();
    out.write((char*)&count, sizeof(int));

    for (const auto& user : m_userDatabase) {
        std::string nameUtf8 = user.name.toStdString();
        int nameLen = nameUtf8.length();
        out.write((char*)&nameLen, sizeof(int));
        out.write(nameUtf8.c_str(), nameLen);

        int featSize = user.feature.size();
        out.write((char*)&featSize, sizeof(int));
        if (featSize > 0) {
            out.write((char*)user.feature.data(), featSize * sizeof(float));
        }
    }
    out.close();

    if (!out.good()) {
        emit logMessage(QStringLiteral("警告: 人脸数据库保存可能不完整: ") + QString::fromStdString(dbPath));
        return;
    }
    emit logMessage(QString("人脸数据库已保存: %1 个用户 %2")
                    .arg(count)
                    .arg(QString::fromStdString(dbPath)));
}

void FaceThread::loadFaceData() {
    const std::string dbPath = faceDatabasePath();
    m_userDatabase.clear();

    const QFileInfo dbInfo(QString::fromStdString(dbPath));
    if (!dbInfo.exists() || dbInfo.size() == 0) {
        saveFaceData();
        emit logMessage(QStringLiteral("人脸数据库已初始化为空库: ") + QString::fromStdString(dbPath));
        return;
    }

    ifstream in(dbPath, ios::binary);
    if (!in.is_open()) {
        emit logMessage(QStringLiteral("警告: 人脸数据库无法打开: ") + QString::fromStdString(dbPath));
        return;
    }

    auto readExact = [&in](char *dst, std::streamsize size) {
        in.read(dst, size);
        return in.gcount() == size;
    };

    int count = 0;
    if (!readExact((char*)&count, sizeof(int)) || count < 0 || count > 256) {
        emit logMessage(QStringLiteral("警告: 人脸数据库头无效，已忽略: ") + QString::fromStdString(dbPath));
        return;
    }

    for (int i = 0; i < count; i++) {
        UserData user;
        int nameLen = 0;
        if (!readExact((char*)&nameLen, sizeof(int)) || nameLen <= 0 || nameLen > 256) {
            emit logMessage(QString("警告: 人脸数据库第 %1 条姓名长度无效，已忽略整个库: %2")
                            .arg(i + 1)
                            .arg(QString::fromStdString(dbPath)));
            m_userDatabase.clear();
            return;
        }

        std::string nameStr(nameLen, '\0');
        if (!readExact(&nameStr[0], nameLen)) {
            emit logMessage(QString("警告: 人脸数据库第 %1 条姓名读取失败，已忽略整个库: %2")
                            .arg(i + 1)
                            .arg(QString::fromStdString(dbPath)));
            m_userDatabase.clear();
            return;
        }
        user.name = QString::fromStdString(nameStr);

        int featSize = 0;
        if (!readExact((char*)&featSize, sizeof(int)) || featSize != FEAT_DIM) {
            emit logMessage(QString("警告: 人脸数据库第 %1 条特征维度无效(%2)，期望 %3，已忽略整个库: %4")
                            .arg(i + 1)
                            .arg(featSize)
                            .arg(FEAT_DIM)
                            .arg(QString::fromStdString(dbPath)));
            m_userDatabase.clear();
            return;
        }

        user.feature.resize(featSize);
        if (!readExact((char*)user.feature.data(), featSize * sizeof(float))) {
            emit logMessage(QString("警告: 人脸数据库第 %1 条特征读取失败，已忽略整个库: %2")
                            .arg(i + 1)
                            .arg(QString::fromStdString(dbPath)));
            m_userDatabase.clear();
            return;
        }
        m_userDatabase.push_back(user);
    }
    in.close();

    emit logMessage(QString("人脸数据库已加载: %1 个用户 %2")
                    .arg(m_userDatabase.size())
                    .arg(QString::fromStdString(dbPath)));
}

std::vector<cv::Vec4f> FaceThread::generatePriors() {
    std::vector<cv::Vec4f> p;
    for (size_t k = 0; k < STRIDES.size(); k++) {
        int stride = STRIDES[k];
        int feature_w = ceil((float)DET_INPUT_W / stride);
        int feature_h = ceil((float)DET_INPUT_H / stride);
        for (int y = 0; y < feature_h; y++) {
            for (int x = 0; x < feature_w; x++) {
                for (int min_size : MIN_SIZES[k]) {
                    float s_kx = min_size / (float)DET_INPUT_W;
                    float s_ky = min_size / (float)DET_INPUT_H;
                    float cx = (x + 0.5f) * stride / DET_INPUT_W;
                    float cy = (y + 0.5f) * stride / DET_INPUT_H;
                    p.emplace_back(cx, cy, s_kx, s_ky);
                }
            }
        }
    }
    return p;
}

cv::Mat FaceThread::alignFace(const cv::Mat &src, const std::vector<cv::Point2f> &landmarks) {
    vector<Point2f> dst_pts;
    for (int i = 0; i < 5; i++)
        dst_pts.push_back(Point2f(REF_POINTS[i][0], REF_POINTS[i][1]));
    Mat M = estimateAffinePartial2D(landmarks, dst_pts);
    Mat aligned;
    warpAffine(src, aligned, M, Size(REC_INPUT_W, REC_INPUT_H));
    return aligned;
}

void FaceThread::normalizeFeature(std::vector<float> &feat) {
    float sum = 0;
    for (float v : feat) sum += v * v;
    float norm = sqrt(sum);
    if (norm > 0) { for (float &v : feat) v /= norm; }
}

float FaceThread::calcSimilarity(const std::vector<float> &f1, const std::vector<float> &f2) {
    if (f1.size() != f2.size()) return 0.0f;
    float sum = 0.0f;
    for (size_t i = 0; i < f1.size(); ++i) sum += f1[i] * f2[i];
    return sum;
}
