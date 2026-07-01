#include "httpconn.h"
using namespace std;

const char *HttpConn::srcDir;	 // 指向网站根目录
atomic<int> HttpConn::userCount; // 计数器，追踪当前服务器上的活动连接数
bool HttpConn::isET;			 // 用于全局设置服务器的epoll模式是边沿触发还是水平触发

// 初始化变量
HttpConn::HttpConn()
{
	fd_ = -1;
	addr_ = {0};
	isClose_ = true;
};

HttpConn::~HttpConn()
{
	Close();
};

// 初始化httpconn对象，使其准备好服务一个新的客户端连接
void HttpConn::init(int fd, const sockaddr_in &addr)
{
	assert(fd > 0); // 确保传入的fd是有效的文件描述符
	userCount++;	// 活动数增加
	addr_ = addr;	// 存储新客户端的地址
	fd_ = fd;		// 存储新客户端的文件描述符
	// 重置读写缓冲区
	writeBuff_.RetrieveAll();
	readBuff_.RetrieveAll();
	isClose_ = false;
	// 记录一条日志，显示新客户端已连接
	LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
}

// 关闭一个活动连接并释放其内存
void HttpConn::Close()
{
	response_.UnmapFile();
	if (isClose_ == false)
	{
		isClose_ = true;
		userCount--;
		close(fd_);
		// 记录客户端断开连接的日志
		LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
	}
}

int HttpConn::GetFd() const
{
	return fd_;
};

struct sockaddr_in HttpConn::GetAddr() const
{
	return addr_;
}

const char *HttpConn::GetIP() const
{
	return inet_ntoa(addr_.sin_addr); // 将二进制的ip地址转换成十进制字符串
}

int HttpConn::GetPort() const
{
	return addr_.sin_port;
}

// fd_读取数据到readBuff_
ssize_t HttpConn::read(int *saveErrno)
{
	ssize_t len = -1;
	do
	{
		len = readBuff_.ReadFd(fd_, saveErrno);
		if (len <= 0)
		{
			break;
		}
	} while (isET); // ET:边沿触发要一次性全部读出（如果是false lt模式，循环只执行一次）
	return len;
}

// 将iov_数组中的数据（头部和正文）写回fd_
ssize_t HttpConn::write(int *saveErrno)
{
	ssize_t len = -1;
	do
	{
		len = writev(fd_, iov_, iovCnt_); // 将iov的内容写到fd中
		if (len <= 0)
		{
			*saveErrno = errno;
			break;
		}
		if (iov_[0].iov_len + iov_[1].iov_len == 0)
		{
			break;
		} /* 传输结束 */
		else if (static_cast<size_t>(len) > iov_[0].iov_len)
		{
			iov_[1].iov_base = (uint8_t *)iov_[1].iov_base + (len - iov_[0].iov_len);
			iov_[1].iov_len -= (len - iov_[0].iov_len);
			if (iov_[0].iov_len)
			{
				writeBuff_.RetrieveAll();
				iov_[0].iov_len = 0;
			}
		}
		else
		{
			iov_[0].iov_base = (uint8_t *)iov_[0].iov_base + len;
			iov_[0].iov_len -= len;
			writeBuff_.Retrieve(len);
		}
	} while (isET || ToWriteBytes() > 10240);
	return len;
}

bool HttpConn::process()
{
	request_.Init();
	if (readBuff_.ReadableBytes() <= 0)
	{
		return false;
	}
	else if (request_.parse(readBuff_))
	{ // 解析成功
		LOG_DEBUG("%s", request_.path().c_str());
		response_.Init(srcDir, request_.path(), request_.IsKeepAlive(), 200);
	}
	else
	{
		response_.Init(srcDir, request_.path(), false, 400);
	}

	response_.MakeResponse(writeBuff_); // 生成响应报文放入writeBuff_中
										// 响应头
	// printf("%.*s\n", (int)writeBuff_.ReadableBytes(), writeBuff_.Peek());

	iov_[0].iov_base = const_cast<char *>(writeBuff_.Peek());
	iov_[0].iov_len = writeBuff_.ReadableBytes();
	iovCnt_ = 1;

	// 文件
	if (response_.FileLen() > 0 && response_.File())
	{
		iov_[1].iov_base = response_.File();
		iov_[1].iov_len = response_.FileLen();
		iovCnt_ = 2;
	}
	LOG_DEBUG("filesize:%d, %d  to %d", response_.FileLen(), iovCnt_, ToWriteBytes());
	return true;
}
