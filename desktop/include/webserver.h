#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <unordered_map>
#include <fcntl.h>	// fcntl()
#include <unistd.h> // close()
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "epoller.h"
#include "heaptimer.h"
#include "log.h"
#include "sqlconnpool.h"
#include "threadpool.h"
#include "httpconn.h"
using namespace std;

// epoll事件调度 heaptimer定时器 threadpool线程池 sqlconnpool数据库连接池
// log日志 httpconn连接管理器
// 采用reactor设计模式： 主线程之负责I/O调度（epoll_wait）和任务分发
//                    threadpool负责业务逻辑

class WebServer
{
public:
	WebServer(
		int port, int trigMode, int timeoutMS, bool OptLinger,
		int sqlPort, const char *sqlUser, const char *sqlPwd,
		const char *dbName, int connPoolNum, int threadNum,
		bool openLog, int logLevel, int logQueSize);

	~WebServer();
	void Start();

    //新增 用于外部强制停止服务器循环
    void Stop() {isClose_ = true;};

private:
	bool InitSocket_();
	void InitEventMode_(int trigMode);
	void AddClient_(int fd, sockaddr_in addr);

	// 事件分发器件
	void DealListen_();
	void DealWrite_(HttpConn *client);
	void DealRead_(HttpConn *client);

	// 连接管理器
	void SendError_(int fd, const char *info);
	void ExtentTime_(HttpConn *client);
	void CloseConn_(HttpConn *client);

	// 任务提交函数，主线程与threadpool之间的桥梁
	void OnRead_(HttpConn *client);
	void OnWrite_(HttpConn *client);
	void OnProcess(HttpConn *client);

	static const int MAX_FD = 65536; // 定义最大fd，即最大并发连接数量

	static int SetFdNonblock(int fd); // 将一个fd设置为非阻塞模式

	int port_;
	bool openLinger_;
	int timeoutMS_; /* 毫秒MS */
	bool isClose_;
	int listenFd_;
    string srcDir_;

	uint32_t listenEvent_; // 监听事件
	uint32_t connEvent_;   // 连接事件

	unique_ptr<HeapTimer> timer_;
	unique_ptr<ThreadPool> threadpool_;
	unique_ptr<Epoller> epoller_;
	unordered_map<int, HttpConn> users_;
};

#endif // WEBSERVER_H
