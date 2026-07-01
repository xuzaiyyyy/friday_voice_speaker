#ifndef LOG_H
#define LOG_H

#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/stat.h>
#include "buffer.h"
#include "blockqueue.h"
#include <functional>
using namespace std;

// 实现一个日志系统（单例模式），同时支持异步和同步两种模式
class Log
{
public:
    //  初始化日志实例（阻塞队列的最大容量、日志保存路径、日志文件后缀）
    void init(int level, const char *path = "../log",
              const char *suffix = ".log",
              int maxQueueCapacity = 1024);

    static Log *Instance();       // 单例模式访问点，static方法，返回全局唯一的Log对象实例
    static void FlushLogThread(); // 异步写日志方法（调用AsyncWrite()）

    void write(int level, const char *format, ...); // 将输出内容按标准格式整理，负责个格式化完整的日志行，并放入buff_缓冲区
    void flush();                                   // 将buff_缓冲区的内容立即处理，两种模式不同的处理方式

    int GetLevel();
    void SetLevel(int level);
    bool IsOpen() { return isOpen_; } // 判断日志系统是否完成初始化

    //新增：定义回调函数类型（接收 std::string参数）
    using LogCallback = function<void(const string&)>;
    //新增：设置回调函数的接口
    void SetCallback(LogCallback cb){callback_  = cb;};

    // // 单例模式，取消拷贝构造和=
    // Log(const Log &) = delete;
    // Log &operator=(const Log &) = delete;

private:
    LogCallback callback_; // 保存回调函数

    bool isOpen_;

    Log();
    virtual ~Log();

    void AppendLogLevelTitle_(int level);
    void AsyncWrite_(); // 异步写日志

    static const int LOG_PATH_LEN = 256;
    static const int LOG_NAME_LEN = 256;
    static const int MAX_LINES = 50000;

    const char *path_;
    const char *suffix_;
    int MAX_LINES_;
    int lineCount_;
    int toDay_;

    buffer buff_; // 输出内容缓冲区
    int level_;
    bool isAsync_; // 是否为异步日志

    FILE *fp_;                             // 打开log的文件指针
    unique_ptr<BlockQueue<string>> deque_; // 阻塞队列
    unique_ptr<thread> writeThread_;       // 写线程的指针
    mutex mtx_;
};

#define LOG_BASE(level, format, ...)                   \
    do                                                 \
    {                                                  \
        Log *log = Log::Instance();                    \
        if (log->IsOpen() && log->GetLevel() <= level) \
        {                                              \
            log->write(level, format, ##__VA_ARGS__);  \
            log->flush();                              \
        }                                              \
    } while (0);

#define LOG_DEBUG(format, ...)             \
    do                                     \
    {                                      \
        LOG_BASE(0, format, ##__VA_ARGS__) \
    } while (0);

#define LOG_INFO(format, ...)              \
    do                                     \
    {                                      \
        LOG_BASE(1, format, ##__VA_ARGS__) \
    } while (0);

#define LOG_WARN(format, ...)              \
    do                                     \
    {                                      \
        LOG_BASE(2, format, ##__VA_ARGS__) \
    } while (0);

#define LOG_ERROR(format, ...)             \
    do                                     \
    {                                      \
        LOG_BASE(3, format, ##__VA_ARGS__) \
    } while (0);

#endif
