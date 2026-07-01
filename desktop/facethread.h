#ifndef FACETHREAD_H
#define FACETHREAD_H

#include <QThread>
#include <QImage>
#include <QDebug>
#include <opencv2/opencv.hpp>
#include <vector>

// 【关键】引入 RKNN API
#include "rknn_api.h"

// 1. 定义人脸检测框结构
struct FaceObject {
    cv::Rect box;
    float score;
    std::vector<cv::Point2f> landmarks;
};

// 2. 定义用户数据结构 (姓名 + 特征)
struct UserData {
    QString name;
    std::vector<float> feature;
};

class FaceThread : public QThread
{
    Q_OBJECT

public:
    explicit FaceThread(QObject *parent = nullptr);
    ~FaceThread();

    // === 控制接口 ===
    void registerFace(QString name); // 录入接口
    void deleteRecord(QString name); // 删除接口

    void startRecognition();
    void stopRecognition();
    void stop();

protected:
    void run() override;

signals:
    void frameProcessed(QImage image);
    void logMessage(QString msg);

private:
    bool m_stop;
    bool m_is_recognizing;

    // 标志位
    bool m_request_register;
    bool m_request_delete;
    QString m_target_name; // 暂存操作姓名

    // 【新增】录入优化相关变量
    int m_register_frame_count;            // 当前已采集帧数
    std::vector<float> m_register_feat_sum; // 特征累加器

    // === NPU 模型句柄 ===
    rknn_context ctx_det; // RetinaFace 检测模型上下文
    rknn_context ctx_rec; // MobileFaceNet 识别模型上下文

    std::vector<cv::Vec4f> priors;

    // 用户数据库
    std::vector<UserData> m_userDatabase;
    QString m_lastRknnError;

    // === 内部算法 (NPU适配版) ===
    int initRKNN(const char* model_path, rknn_context* ctx);
    void releaseRKNN();

    void loadFaceData();
    void saveFaceData();
    std::vector<cv::Vec4f> generatePriors();

    // NPU 推理函数
    std::vector<FaceObject> detectFacesNPU(cv::Mat &frame);
    std::vector<float> extractFeatureNPU(cv::Mat &aligned);

    // 辅助算法
    cv::Mat alignFace(const cv::Mat &src, const std::vector<cv::Point2f> &landmarks);
    float calcSimilarity(const std::vector<float> &f1, const std::vector<float> &f2);
    void normalizeFeature(std::vector<float> &feat);
};

#endif // FACETHREAD_H
