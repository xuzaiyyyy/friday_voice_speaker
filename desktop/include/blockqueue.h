#pragma
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <sys/time.h>
using namespace std;

// 实现一个模板化、线程安全的阻塞队列

template <typename T>
class BlockQueue
{
public:
    explicit BlockQueue(size_t maxsize = 1000); // 创造一个最大容量为maxsize的阻塞队列。利用explicit关键字防止隐式类型转换
    ~BlockQueue();                              // 析构函数，在队列对象销毁时自动调用Close（），确保清理资源并唤醒所有可能在等待的线程

    bool empty();                   // 线程安全地检查队列是否为空
    bool full();                    // 线程安全地检查队列是否已满
    void push_back(const T &item);  // 【生产者】从队尾添加一个元素。如果队列已满，此线程阻塞，直到有消费者取出元素
    void push_front(const T &item); // 【生产者】从队头添加一个元素。如果队列已满，此线程阻塞，直到有消费者取出元素
    bool pop(T &item);              // 【消费者】从队头取出一个元素。如果队列为空，此线程阻塞，直到有生产者添加元素
    bool pop(T &item, int timeout); // 【消费者】pop带超时版本，会等待timeout秒，如果超时，直接返回false
    void clear();
    T front(); // 线程安全地获取队列的第一个元素，但不从队列中移除
    T back();
    size_t capacity();
    size_t size();
    void flush(); // 唤醒一个正在等待的消费者线程
    void Close(); // 关闭队列

private:
    deque<T> deq_; // 底层数据结构
    mutex mtx_;
    bool isClose_;    // 关闭标志
    size_t capacity_; // 容量
    condition_variable condConsumer_;
    condition_variable condProducer_;
};

// 构造函数初始化capacity_为maxsize，isClose为false
template <typename T>
BlockQueue<T>::BlockQueue(size_t maxsize) : capacity_(maxsize)
{
    assert(maxsize > 0);
    isClose_ = false;
}

// 析构函数，在队列对象销毁时自动调用Close（），确保清理资源并唤醒所有可能在等待的线程
template <typename T>
BlockQueue<T>::~BlockQueue()
{
    Close();
}

// 关闭队列
template <typename T>
void BlockQueue<T>::Close()
{
    clear(); // 首先调用clear（）清空队列
    isClose_ = true;
    // 唤醒所有的消费者和生产者，防止永久阻塞
    condConsumer_.notify_all();
    condProducer_.notify_all();
}

// 这里加锁是保证操作的原子性，防止与push_back或pop等操作发生冲突
template <typename T>
void BlockQueue<T>::clear()
{
    lock_guard<mutex> locker(mtx_);
    deq_.clear(); // 清空队列
}

// 同样加锁保证操作的原子性
template <typename T>
bool BlockQueue<T>::empty()
{
    lock_guard<mutex> locker(mtx_);
    return deq_.empty(); // 判断队列是否为空
}

template <typename T>
bool BlockQueue<T>::full()
{
    lock_guard<mutex> locker(mtx_);
    return deq_.size() >= capacity_; // 判断队列元素是否超过最大容量
}

template <typename T>
void BlockQueue<T>::push_back(const T &item)
{
    unique_lock<mutex> locker(mtx_);
    while (deq_.size() >= capacity_)
    {
        condProducer_.wait(locker); // 若队列已满，则释放掉锁，等有消费者取出数据，重新唤醒
    }
    deq_.push_back(item);
    condConsumer_.notify_one();
}

template <typename T>
void BlockQueue<T>::push_front(const T &item)
{
    unique_lock<mutex> locker(mtx_);
    while (deq_.size() >= capacity_)
    {
        condProducer_.wait(locker); // 若队列已满，则释放掉锁，等有消费者取出数据，重新唤醒
    }
    deq_.push_front(item);
    condConsumer_.notify_one();
}

template <typename T>
bool BlockQueue<T>::pop(T &item)
{
    unique_lock<mutex> locker(mtx_);
    while (deq_.empty())
    {
        condProducer_.wait(locker); // 若队列为空，则释放掉锁，等有生产者传入数据，重新唤醒

        if (isClose_)
        {
            return false;
        }
    }
    item = deq_.front(); // 首先获得队列中的第一个元素
    deq_.pop_front();    // 并将这个元素移出队列
    condProducer_.notify_one();
    return true;
}

template <typename T>
bool BlockQueue<T>::pop(T &item, int timeout)
{
    unique_lock<mutex> locker(mtx_);
    while (deq_.empty())
    {
        // 1 首先等待指定时间
        if (condConsumer_.wait_for(locker, chrono::seconds(timeout)) == cv_status::timeout)
        {
            return false; // 如果是超时性来，返回false
        }
        if (isClose_)
        {
            return false;
        }
    }
    item = deq_.front();  // 首先获得队列中的第一个元素
    deq_.pop_front(item); // 并将这个元素移出队列
    condProducer_.notify_one();
    return true;
}

template <typename T>
T BlockQueue<T>::front()
{
    lock_guard<mutex> locker(mtx_);
    // 取出元素的同时应该加个判断，队列是否为空
    if (deq_.empty())
    {
        throw runtime_error("BlockQueue::front() called on empty queue!");
    }
    return deq_.front();
}

template <typename T>
T BlockQueue<T>::back()
{
    lock_guard<mutex> locker(mtx_);
    // 取出元素的同时应该加个判断，队列是否为空
    if (deq_.empty())
    {
        throw runtime_error("BlockQueue::back() called on empty queue!");
    }
    return deq_.back();
}

template <typename T>
size_t BlockQueue<T>::capacity()
{
    lock_guard<mutex> locker(mtx_);
    return capacity_;
}

// 唤醒一个正在等待的消费者线程
template <typename T>
void BlockQueue<T>::flush()
{
    condConsumer_.notify_one();
}