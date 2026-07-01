#ifndef EPOLLER_H
#define EPOLLER_H

#include <sys/epoll.h> //epoll_ctl()
#include <unistd.h>	   // close()
#include <assert.h>	   // close()
#include <vector>
#include <errno.h>
using namespace std;

class Epoller
{
public:
	explicit Epoller(int maxEvent = 1024); // 参数指定了epoll_wait一次最多能返回多少个事件
	~Epoller();

	bool AddFd(int fd, uint32_t events); // 向epoll实例中添加一个新的fd进行监听
	bool ModFd(int fd, uint32_t events); // 修改一个已经注册的fd所监听的事件
	bool DelFd(int fd);					 // 从epoll中删除一个fd
	int Wait(int timeoutMs = -1);		 // 阻塞主线程，直到有fd触发了事件
	int GetEventFd(size_t i) const;		 // 返回第i个就绪事件的fd
	uint32_t GetEvents(size_t i) const;	 // 返回第i个就绪事件类型

private:
	int epollFd_;						// epoll实例的文件描述符
	vector<struct epoll_event> events_; // 结果向量
};

#endif // EPOLLER_H
