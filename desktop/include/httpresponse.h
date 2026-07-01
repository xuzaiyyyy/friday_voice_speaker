#pragma
#include <unordered_map>
#include <fcntl.h>	  // open
#include <unistd.h>	  // close
#include <sys/stat.h> // stat
#include <sys/mman.h> // mmap, munmap
#include "buffer.h"
#include "log.h"
using namespace std;

class HttpResponse
{
public:
	HttpResponse();
	~HttpResponse();

	void Init(const string &srcDir, string &path, bool isKeepAlive = false, int code = -1);
	void MakeResponse(buffer &buff); // 将状态行和头部写入buff中；他不会将文件内容mmFile_写入buff中
	void UnmapFile();				 // 调用munmap来释放mmFile_指向的内存映射
	char *File();
	size_t FileLen() const;
	void ErrorContent(buffer &buff, string message); // 当init失败，在buff中生成一个html格式的错误页面
	int Code() const { return code_; }				 // 返回最终的HTTP状态码

private:
	void AddStateLine_(buffer &buff); // 向buff中添加状态行
	void AddHeader_(buffer &buff);	  // 向buff中添加所有头部
	void AddContent_(buffer &buff);	  // 添加错误页面的内容

	void ErrorHtml_();
	string GetFileType_(); // 根据path_的后缀，在SUFFIX_TYPE映射表中查找并返回对应的MIME类型

	int code_; // 存储http状态玛
	bool isKeepAlive_;

	string path_;
	string srcDir_; // 网站的根目录

	char *mmFile_; // 指向mmap映射到内存中的文件内容的起始地址
	struct stat mmFileStat_;

	static const unordered_map<string, string> SUFFIX_TYPE; // 后缀类型集
	static const unordered_map<int, string> CODE_STATUS;	// 编码状态集
	static const unordered_map<int, string> CODE_PATH;		// 编码路径集
};
