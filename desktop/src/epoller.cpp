#include "epoller.h"
using namespace std;

// 构造函数，初始化epoller
// 初始化列表epoll_create向操作系统内核请求创建一个epoll实例，内核会返回一个文件描述符fd，代表epoll实例，并存放在epollFd_中
// 预先分配了maxEvent个epoll_event结构体的空间
// 这个event_ vector是用来接受就绪事件的，epoll_wait将把就绪的事件复制到这个vector中
Epoller::Epoller(int maxEvent) : epollFd_(epoll_create(512)), events_(maxEvent)
{
	assert(epollFd_ >= 0 && events_.size() > 0);
}

// 析构函数，用来清理资源
Epoller::~Epoller()
{
	close(epollFd_);
}

// 向epoll实例中添加一个新的文件描述符
bool Epoller::AddFd(int fd, uint32_t events)
{
	if (fd < 0)
		return false;
	epoll_event ev = {0}; // 创建一个epoll_event结构体，并告诉epoll这个事件与fd相关联
	ev.data.fd = fd;
	ev.events = events;
	return 0 == epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
}

// 修改一个已注册的fd所监听的事件
// 这是服务器状态切换的关键
// 用来切换监听可读或者可写的事件
bool Epoller::ModFd(int fd, uint32_t events)
{
	if (fd < 0)
		return false;
	epoll_event ev = {0};
	ev.data.fd = fd;
	ev.events = events;
	return 0 == epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
}

// 删除一个fd
bool Epoller::DelFd(int fd)
{
	if (fd < 0)
		return false;
	return 0 == epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, 0);
}

// 返回事件数量
// 主循环根据这个数量去遍历events_数组的前N个元素
int Epoller::Wait(int timeoutMs)
{
	return epoll_wait(epollFd_, &events_[0], static_cast<int>(events_.size()), timeoutMs);
}

// 获取第i个事件的fd
int Epoller::GetEventFd(size_t i) const
{
	assert(i < events_.size() && i >= 0);
	return events_[i].data.fd;
}

// 获取第i个事件属性
uint32_t Epoller::GetEvents(size_t i) const
{
	assert(i < events_.size() && i >= 0);
	return events_[i].events;
}
