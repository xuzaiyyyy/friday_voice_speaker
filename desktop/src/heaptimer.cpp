#include "heaptimer.h"
using namespace std;

// 交换堆中索引为i和j的两个节点
void HeapTimer::SwapNode_(size_t i, size_t j)
{
	assert(i >= 0 && i < heap_.size());
	assert(j >= 0 && j < heap_.size());
	swap(heap_[i], heap_[j]);
	ref_[heap_[i].id] = i; // ref_映射表中的位置状态要同步
	ref_[heap_[j].id] = j;
}

// 向上调整。将索引i处的节点向上移动，直到满足最小堆的性质（它的过期时间晚于或等于其父节点）
// 这里使用的数组模拟二叉树
void HeapTimer::siftup_(size_t i)
{
	assert(i >= 0 && i < heap_.size());
	size_t parent = (i - 1) / 2;
	while (i > 0)
	{
		if (heap_[parent] > heap_[i])
		{
			SwapNode_(i, parent);
			i = parent;
			parent = (i - 1) / 2;
		}
		else
		{
			break;
		}
	}
}

// 向上调整。将索引i处的节点向下移动，直到满足最小堆的性质（它的过期时间早于或等于其子节点）
bool HeapTimer::siftdown_(size_t i, size_t n)
{
	assert(i >= 0 && i < heap_.size());
	assert(n >= 0 && n <= heap_.size()); // n:共几个结点
	auto index = i;
	auto child = 2 * index + 1;
	while (child < n)
	{
		if (child + 1 < n && heap_[child + 1] < heap_[child])
		{
			child++;
		}
		if (heap_[child] < heap_[index])
		{
			SwapNode_(index, child);
			index = child;
			child = 2 * child + 1;
		}
		break; // 需要跳出循环
	}
	return index > i;
}

// 删除指定位置的结点
void HeapTimer::del_(size_t index)
{
	assert(index >= 0 && index < heap_.size());
	// 将要删除的结点换到队尾，然后调整堆
	size_t tmp = index;
	size_t n = heap_.size() - 1;
	assert(tmp <= n);
	// 如果就在队尾，就不用移动了
	if (index < heap_.size() - 1)
	{
		SwapNode_(tmp, heap_.size() - 1);
		if (!siftdown_(tmp, n))
		{
			siftup_(tmp);
		}
	}
	ref_.erase(heap_.back().id);
	heap_.pop_back();
}

// 调整已存在定时器的超时时间
void HeapTimer::adjust(int id, int newExpires)
{
	assert(!heap_.empty() && ref_.count(id));
	heap_[ref_[id]].expires = Clock::now() + MS(newExpires);
	siftdown_(ref_[id], heap_.size());
}

// 添加一个新的定时器。如果id已存在，则等同于adjust
void HeapTimer::add(int id, int timeOut, const TimeoutCallBack &cb)
{
	assert(id >= 0);
	// 如果有，则调整
	if (ref_.count(id))
	{
		int tmp = ref_[id];
		heap_[tmp].expires = Clock::now() + MS(timeOut);
		heap_[tmp].cb = cb;
		if (!siftdown_(tmp, heap_.size()))
		{
			siftup_(tmp);
		}
	}
	else
	{
		size_t n = heap_.size();
		ref_[id] = n;
		// 这里应该算是结构体的默认构造？
		heap_.push_back({id, Clock::now() + MS(timeOut), cb}); // 右值
		siftup_(n);
	}
}

// 立即执行id对应的回调函数，并将其从定时器中删除
void HeapTimer::doWork(int id)
{
	if (heap_.empty() || ref_.count(id) == 0)
	{
		return;
	}
	size_t i = ref_[id];
	auto node = heap_[i];
	node.cb(); // 触发回调函数
	del_(i);
}

// 心跳函数，检查并处理所有已到期的定时器
void HeapTimer::tick()
{
	/* 清除超时结点 */
	if (heap_.empty())
	{
		return;
	}
	while (!heap_.empty())
	{
		TimerNode node = heap_.front();
		if (chrono::duration_cast<MS>(node.expires - Clock::now()).count() > 0)
		{
			break;
		}
		node.cb();
		pop();
	}
}

void HeapTimer::pop()
{
	assert(!heap_.empty());
	del_(0);
}

void HeapTimer::clear()
{
	ref_.clear();
	heap_.clear();
}

// 获取下一个定时器将在多少ms后到期，这是epoll_wait超时时间的关键
int HeapTimer::GetNextTick()
{
	tick();
	size_t res = -1;
	if (!heap_.empty())
	{
		res = chrono::duration_cast<MS>(heap_.front().expires - Clock::now()).count();
		if (res < 0)
		{
			res = 0;
		}
	}
	return res;
}
