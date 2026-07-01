#include "httpresponse.h"
using namespace std;

// 后缀类型集
const unordered_map<string, string> HttpResponse::SUFFIX_TYPE = {
	{".html", "text/html"},
	{".xml", "text/xml"},
	{".xhtml", "application/xhtml+xml"},
	{".txt", "text/plain"},
	{".rtf", "application/rtf"},
	{".pdf", "application/pdf"},
	{".word", "application/nsword"},
	{".png", "image/png"},
	{".gif", "image/gif"},
	{".jpg", "image/jpeg"},
	{".jpeg", "image/jpeg"},
	{".au", "audio/basic"},
	{".mpeg", "video/mpeg"},
	{".mpg", "video/mpeg"},
	{".avi", "video/x-msvideo"},
	{".gz", "application/x-gzip"},
	{".tar", "application/x-tar"},
	{".css", "text/css "},
	{".js", "text/javascript "},
};

// 状态玛映射表
const unordered_map<int, string> HttpResponse::CODE_STATUS = {
	{200, "OK"},
	{400, "Bad Request"},
	{403, "Forbidden"},
	{404, "Not Found"},
};

// 错误页面映射表，将错误状态玛映射到服务器上对应的错误页面文件
const unordered_map<int, string> HttpResponse::CODE_PATH = {
	{400, "/400.html"},
	{403, "/403.html"},
	{404, "/404.html"},
};

// 初始化所有成员变量
HttpResponse::HttpResponse()
{
	code_ = -1; //=-1表示未设置
	path_ = srcDir_ = "";
	isKeepAlive_ = false;
	mmFile_ = nullptr;
	mmFileStat_ = {0};
};

HttpResponse::~HttpResponse()
{
	UnmapFile();
}

// 新的请求信息覆盖旧信息
void HttpResponse::Init(const string &srcDir, string &path, bool isKeepAlive, int code)
{
	assert(srcDir != "");
	if (mmFile_)
	{
		UnmapFile();
	}
	code_ = code;
	isKeepAlive_ = isKeepAlive;
	path_ = path;
	srcDir_ = srcDir;
	mmFile_ = nullptr;
	mmFileStat_ = {0};
}

// 用于构建完整的http头部
void HttpResponse::MakeResponse(buffer &buff)
{
	// 首先判断文件或者目录是否存在
	if (stat((srcDir_ + path_).data(), &mmFileStat_) < 0 || S_ISDIR(mmFileStat_.st_mode))
	{
		code_ = 404;
	}
	// 判断文件权限
	else if (!(mmFileStat_.st_mode & S_IROTH))
	{
		code_ = 403;
	}
	// 一切正常，设置为200
	else if (code_ == -1)
	{
		code_ = 200;
	}
	ErrorHtml_(); // 如果code_是404或者403，他会修改path_变量，并重新调用stat来获取错误页面
	AddStateLine_(buff);
	AddHeader_(buff);
	AddContent_(buff);
}

char *HttpResponse::File()
{
	return mmFile_;
}

size_t HttpResponse::FileLen() const
{
	return mmFileStat_.st_size;
}

// 如果发生了错误（如404），将服务器要发送的不存在的文件切换到准备好的错误页面文件
void HttpResponse::ErrorHtml_()
{
	if (CODE_PATH.count(code_) == 1)
	{
		path_ = CODE_PATH.find(code_)->second;
		stat((srcDir_ + path_).data(), &mmFileStat_);
	}
}

// 构建状态行
void HttpResponse::AddStateLine_(buffer &buff)
{
	string status;
	if (CODE_STATUS.count(code_) == 1)
	{
		status = CODE_STATUS.find(code_)->second;
	}
	else
	{
		code_ = 400;
		status = CODE_STATUS.find(400)->second;
	}
	string line = "HTTP/1.1 " + to_string(code_) + " " + status + "\r\n";
	buff.Append(line);

	// printf("debug: %s", line.c_str());
	// printf("debug:%zu\n", buff.ReadableBytes());
}

// 构建状态头
void HttpResponse::AddHeader_(buffer &buff)
{
	buff.Append("Connection: ");
	if (isKeepAlive_)
	{
		buff.Append("keep-alive\r\n");
		buff.Append("keep-alive: max=6, timeout=120\r\n");
	}
	else
	{
		buff.Append("close\r\n");
	}
	buff.Append("Content-type: " + GetFileType_() + "\r\n");
}

// 准备文件正文
void HttpResponse::AddContent_(buffer &buff)
{
	int srcFd = open((srcDir_ + path_).data(), O_RDONLY);
	if (srcFd < 0)
	{
		ErrorContent(buff, "File NotFound!");
		return;
	}

	// 将文件映射到内存提高文件的访问速度  MAP_PRIVATE 建立一个写入时拷贝的私有映射
	LOG_DEBUG("file path %s", (srcDir_ + path_).data());
	void *mmRet = mmap(0, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0);
	if (mmRet == MAP_FAILED)
	{
		ErrorContent(buff, "File NotFound!");
		return;
	}
	mmFile_ = (char *)mmRet;
	close(srcFd);
	buff.Append("Content-length: " + to_string(mmFileStat_.st_size) + "\r\n\r\n");
}

// 释放mmap资源
void HttpResponse::UnmapFile()
{
	if (mmFile_)
	{
		munmap(mmFile_, mmFileStat_.st_size);
		mmFile_ = nullptr;
	}
}

// 判断文件类型
string HttpResponse::GetFileType_()
{
	string::size_type idx = path_.find_last_of('.');
	if (idx == string::npos)
	{ // 最大值 find函数在找不到指定值得情况下会返回string::npos
		return "text/plain";
	}
	string suffix = path_.substr(idx);
	if (SUFFIX_TYPE.count(suffix) == 1)
	{
		return SUFFIX_TYPE.find(suffix)->second;
	}
	return "text/plain";
}

// 备用的错误页面生成器
void HttpResponse::ErrorContent(buffer &buff, string message)
{
	string body;
	string status;
	body += "<html><title>Error</title>";
	body += "<body bgcolor=\"ffffff\">";
	if (CODE_STATUS.count(code_) == 1)
	{
		status = CODE_STATUS.find(code_)->second;
	}
	else
	{
		status = "Bad Request";
	}
	body += to_string(code_) + " : " + status + "\n";
	body += "<p>" + message + "</p>";
	body += "<hr><em>TinyWebServer</em></body></html>";

	buff.Append("Content-length: " + to_string(body.size()) + "\r\n\r\n");
	buff.Append(body);
}
