#pragma
#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <assert.h>
using namespace std;

class ThreadPool
{
public:
    ThreadPool() = default;              // 使用默认构造函数
    ThreadPool(ThreadPool &&) = default; // 显式声明使用编译器自动生成的移动构造函数，允许线程池对象被move转移

    // 主构造函数，尽量用make_shared代替new，如果通过new再传给shared_ptr，内存是不连续的，会造成内存碎片化
    explicit ThreadPool(int threadCount = 8) : pool_(make_shared<Pool>()) {
        assert(threadCount > 0);
        for (int i = 0; i < threadCount; i++) {
            // 🚨 修改点1：不再 detach，而是把线程存入 vector 管理
            threads_.emplace_back([this]() {
                unique_lock<mutex> locker(pool_->mtx_);
                while (true) {
                    if (!pool_->tasks.empty()) {
                        auto task = move(pool_->tasks.front());
                        pool_->tasks.pop();
                        locker.unlock();
                        task();
                        locker.lock();
                    } else if (pool_->isClosed) {
                        break;
                    } else {
                        pool_->cond_.wait(locker);
                    }
                }
            });
        }
    }

    ~ThreadPool() {
        if (pool_) {
            {
                unique_lock<mutex> locker(pool_->mtx_);
                pool_->isClosed = true;
            } // 记得先解锁再通知，虽然 notify_all 内部不持锁也行，但这是好习惯
            pool_->cond_.notify_all();
        }

        // 🚨 修改点2：等待所有线程安全退出 (Join)
        for (std::thread &th : threads_) {
            if (th.joinable()) {
                th.join();
            }
        }
    }

    template <typename T>
    void AddTask(T &&task)
    {
        unique_lock<mutex> locker(pool_->mtx_);
        pool_->tasks.emplace(forward<T>(task)); // 在队尾原地构造funtcion对象
        pool_->cond_.notify_one();              // 添加了一个任务，所以唤醒一个线程来处理
    }

private:
    struct Pool
    {
        mutex mtx_;
        condition_variable cond_;
        bool isClosed;
        queue<function<void()>> tasks; // 任务队列，函数类型为void()
    };
    shared_ptr<Pool> pool_;

    // 🚨 修改点3：新增线程容器
    std::vector<std::thread> threads_;
};
