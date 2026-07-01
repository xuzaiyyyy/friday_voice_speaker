#ifndef HEAP_TIMER_H
#define HEAP_TIMER_H

#include <queue>
#include <unordered_map>
#include <time.h>
#include <algorithm>
#include <arpa/inet.h>
#include <functional>
#include <assert.h>
#include <chrono>
#include "log.h"
using namespace std;

typedef function<void()> TimeoutCallBack;	 // 定时期到期执行的回调函数
typedef chrono::high_resolution_clock Clock; // 类型别名，高精度时钟
typedef chrono::milliseconds MS;			 // 毫秒
typedef Clock::time_point TimeStamp;		 // 具体时间点

// 存储在堆中的基本单元（节点）
struct TimerNode
{
	int id;				// 定时器id
	TimeStamp expires;	// 超时时间点
	TimeoutCallBack cb; // 回调function<void()>
	bool operator<(const TimerNode &t)
	{ // 重载比较运算符
		return expires < t.expires;
	}
	bool operator>(const TimerNode &t)
	{ // 重载比较运算符
		return expires > t.expires;
	}
};

class HeapTimer
{
public:
	HeapTimer() { heap_.reserve(64); } // 保留（扩充）容量
	~HeapTimer() { clear(); }

	void adjust(int id, int newExpires);					  // 调整一个已存在定时器的超时时间
	void add(int id, int timeOut, const TimeoutCallBack &cb); // 添加一个全新的定时器
	void doWork(int id);									  // 立即执行一个定时器的回调
	void clear();
	void tick(); // 心跳函数，检查并处理所有已到期的定时器
	void pop();
	int GetNextTick(); // 计算下一个定时器将在多少ms后到期

private:
	void del_(size_t i);
	void siftup_(size_t i);
	bool siftdown_(size_t i, size_t n);
	void SwapNode_(size_t i, size_t j);

	vector<TimerNode> heap_;
	// key : id value : vector的下标
	unordered_map<int, size_t> ref_; // id对应的在heap_中的下标，方便用heap_的时候查找
};

#endif
