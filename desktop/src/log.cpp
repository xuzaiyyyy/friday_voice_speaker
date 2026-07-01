#include "log.h"
using namespace std;

// 构造函数，将所有成员变量初始化为 空 或 零
Log::Log()
{
    fp_ = nullptr;          // 文件指针
    deque_ = nullptr;       // 阻塞队列
    writeThread_ = nullptr; // 写线程
    lineCount_ = 0;
    toDay_ = 0;
    isAsync_ = false;
}

Log::~Log()
{
    while (!deque_->empty())
    {
        deque_->flush();
    }
    deque_->Close();
    writeThread_->join();
    if (fp_)
    {
        lock_guard<mutex> locker(mtx_);
        flush();
        fclose(fp_); // 关闭日志文件
    }
}

// 唤醒阻塞队列消费者，开始写日志
void Log::flush()
{
    if (isAsync_) // 首先判断是否为异步
    {
        deque_->flush();
    }
    fflush(fp_);
}

// 懒汉模式 局部静态变量法（不需要加锁 解锁操作）,直到第一次调用Log::Instance()时，Log对象才被创建
Log *Log::Instance()
{
    static Log log;
    return &log;
}

// 异步日志的写线程函数
void Log::FlushLogThread()
{
    Log::Instance()->AsyncWrite_();
}

void Log::AsyncWrite_()
{
    string str = "";
    while (deque_->pop(str)) // 将队列中的信息存入到str
    {
        lock_guard<mutex> locker(mtx_);
        fputs(str.c_str(), fp_);
        // fwrite(str.c_str(), 1, str.length(), fp_);
        //fflush(fp_);//实时更新日志，不需要存满4kb
    }
}

// 初始化日志实例
void Log::init(int level, const char *path, const char *suffix, int maxQueueCapacity)
{
    isOpen_ = true;
    level_ = level;
    path_ = path;
    suffix_ = suffix;
    if (maxQueueCapacity > 0) // 如果队列容量大于0，则使用异步模式
    {
        isAsync_ = true;
        if (!deque_) // 检查队列是否已初始化
        {
            unique_ptr<BlockQueue<string>> newQue(new BlockQueue<string>);
            deque_ = move(newQue);
            unique_ptr<thread> newThread(new thread(FlushLogThread)); // 创建线程，并立即执行FlushLogThread
            writeThread_ = move(newThread);
        }
    }
    else
    {
        isAsync_ = false;
    }

    lineCount_ = 0;
    time_t timer = time(nullptr); // 只通过time的返回值返回当前时间
    struct tm *systime = localtime(&timer);
    char fileName[LOG_NAME_LEN] = {0};
    snprintf(fileName, LOG_NAME_LEN - 1, "%s/%04d_%02d_%02d%s", path_, systime->tm_year + 1900, systime->tm_mon + 1, systime->tm_mday, suffix_);
    toDay_ = systime->tm_mday; // 保存今天是几号，用于后续的日志分割检查

    {
        lock_guard<mutex> locker(mtx_);
        buff_.RetrieveAll(); // 这一步的作用是清空缓冲区
        if (fp_)             // 如果init被重复调用，安全地关闭旧的fp_
        {
            flush();
            fclose(fp_);
        }
        fp_ = fopen(fileName, "a"); // 以追加模式打开日志文件
        if (fp_ == nullptr)         // 若目录不存在
        {
            mkdir(path_, 0777); // 首先创建目标文件夹
            fp_ = fopen(fileName, "a");
        }
        assert(fp_ != nullptr); // 确保两次打开文件成功了
    }
}

void Log::write(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, nullptr);
    time_t tSec = now.tv_sec;
    struct tm *sysTime = localtime(&tSec);
    struct tm t = *sysTime;
    va_list vaList;

    // 日志日期 日志行数 如果不是今天或行数超了
    if (toDay_ != t.tm_mday || (lineCount_ && (lineCount_ % MAX_LINES == 0)))
    {
        unique_lock<mutex> locker(mtx_);
        locker.unlock();

        char newFile[LOG_NAME_LEN];
        char tail[36] = {0};
        snprintf(tail, 36, "%04d_%02d_%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        if (toDay_ != t.tm_mday) // 若时间不匹配，则替换成最新的日志文件名
        {
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s%s", path_, tail, suffix_);
            toDay_ = t.tm_mday;
            lineCount_ = 0;
        }
        else
        {
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s-%d%s", path_, tail, (lineCount_ / MAX_LINES), suffix_);
        }

        locker.lock();
        flush();
        fclose(fp_);
        fp_ = fopen(newFile, "a");
        assert(fp_ != nullptr);
    }

    // 在buffer内生成一条对应的日志信息
    {
        unique_lock<mutex> locker(mtx_);
        lineCount_++;
        int n = snprintf(buff_.BeginWrite(), 128, "%d-%02d-%02d %02d:%02d:%02d.%06ld",
                         t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                         t.tm_min, t.tm_sec, now.tv_usec);
        buff_.HasWriten(n); // 移动写指针（事件辍长度）

        AppendLogLevelTitle_(level); // 写入日志等级

        va_start(vaList, format);

        int m = vsnprintf(buff_.BeginWrite(), buff_.WritableBytes(), format, vaList);

        va_end(vaList);

        buff_.HasWriten(m);

        buff_.Append("\n", 1);

        //新增代码
        //如果注册了回调函数，就把当前的日志内容发出去
        //使用Peek()获取内容，ReadableBytes()获取长度
        if(callback_)
        {
            string currentLog(buff_.Peek(), buff_.ReadableBytes());
            callback_(currentLog);
        }

        if (isAsync_ && deque_ && !deque_->full()) // 异步方式（加入阻塞队列中，等待写线程读取日志信息）
        {
            deque_->push_back(buff_.RetrieveAllToStr());
        }
        else // 同步方式（直接向文件中写入日志信息）
        {
            fputs(buff_.Peek(), fp_);
            //fflush(fp_);
            buff_.RetrieveAll();
        }

    }
}

// 添加日志级别前缀
void Log::AppendLogLevelTitle_(int level)
{
    switch (level) // 根据日志级别选择对应的前缀
    {
    case 0:
        buff_.Append("[debug]: ", 9); // 调试前缀
        break;
    case 1:
        buff_.Append("[info] :  ", 9); // 信息前缀
        break;
    case 2:
        buff_.Append("[warn] :  ", 9); // 警告前缀
        break;
    case 3:
        buff_.Append("[erro] :  ", 9); // 错误前缀
        break;
    default: // 未知级别，默认使用信息级别
        buff_.Append("[info] :  ", 9);
        break;
    }
}

// 获取当前日志级别
int Log::GetLevel()
{
    lock_guard<mutex> locker(mtx_);
    return level_;
}

// 设置日志级别
void Log::SetLevel(int level)
{
    lock_guard<mutex> locker(mtx_);
    level_ = level;
}
