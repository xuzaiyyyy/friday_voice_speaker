#pragma
#include <sys/types.h>
#include <sys/uio.h>   // readv/writev
#include <arpa/inet.h> // sockaddr_in
#include <stdlib.h>	   // atoi()
#include <errno.h>
#include "httprequest.h"
#include "httpresponse.h"
using namespace std;
/*
 * 进行读写数据并调用httprequest 来解析数据以及httpresponse来生成响应
 * */
class HttpConn
{
public:
	HttpConn();
	~HttpConn();

	void init(int sockFd, const sockaddr_in &addr);
	ssize_t read(int *saveErrno);
	ssize_t write(int *saveErrno);
	void Close();
	int GetFd() const;
	int GetPort() const;
	const char *GetIP() const;
	sockaddr_in GetAddr() const;
	bool process();

	// 写的总长度
	int ToWriteBytes()
	{
		return iov_[0].iov_len + iov_[1].iov_len;
	}

	bool IsKeepAlive() const
	{
		return request_.IsKeepAlive();
	}

	static bool isET;
	static const char *srcDir;
	static atomic<int> userCount; // 原子，支持锁

private:
	int fd_;				  // 此连接的套接字文件描述符
	struct sockaddr_in addr_; // 客户端的地址信息

	bool isClose_;

	int iovCnt_;
	struct iovec iov_[2];

	buffer readBuff_;  // 读缓冲区
	buffer writeBuff_; // 写缓冲区

	HttpRequest request_;	// 请求解析器
	HttpResponse response_; // 响应解析器
};
