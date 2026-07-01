#include "httprequest.h"
using namespace std;

// DEFAULT_HTML这是一个set集合，定义了一组默认的HTML页面名称（不带.html后缀）
// 它的作用是在ParsePath_函数中，如果请求的路径在这个集合中，程序会自动为其追加.html后缀
const unordered_set<string> HttpRequest::DEFAULT_HTML{
	"/index",
	"/register",
	"/login",
	"/welcome",
	"/video",
	"/picture",
};

// DEFAULT_HTML_TAG是一个map（映射表），它为特定的页面分配一个数字标签
// 这个标签（0 1）会在ParsePost_函数中被用来区分当前请求是注册（0）还是登陆（1）
const unordered_map<string, int> HttpRequest::DEFAULT_HTML_TAG{
	{"/register.html", 0},
	{"/login.html", 1},
};

// 重置HttpRequest对象到初始状态
void HttpRequest::Init()
{
	method_ = path_ = version_ = body_ = "";
	state_ = REQUEST_LINE;
	header_.clear();
	post_.clear();
}

// 判断客户端是否请求保持长连接
bool HttpRequest::IsKeepAlive() const
{
	if (header_.count("Connection") == 1)
	{
		return header_.find("Connection")->second == "keep-alive" && version_ == "1.1"; // HTTP/1.1默认是keep-alive
	}
	return false;
}

// 解析处理（有限状态机）
bool HttpRequest::parse(buffer &buff)
{
	const char CRLF[] = "\r\n";	   // 行结束符标志(回车换行)
	if (buff.ReadableBytes() <= 0) // 如果缓冲区没有可读数据
	{
		return false;
	}
	// 只要有数据可读，且状态机未完成
	while (buff.ReadableBytes() && state_ != FINISH)
	{
		// 从buff中的读指针开始到读指针结束，这块区域是未读取得数据并去除"\r\n"，返回有效数据得行末指针
		// 这就相当于在找 \r\n 如果找到，则说明找到一行
		const char *lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
		// 转化为string类型
		string line(buff.Peek(), lineEnd);
		switch (state_)
		{
			/*
			有限状态机，从请求行开始，每处理完后会自动转入到下一个状态
			*/
		case REQUEST_LINE: // 状态1 解析请求行
			if (!ParseRequestLine_(line))
			{
				return false; // 解析失败，返回错误
			}
			ParsePath_(); // 解析路径，例如将 / 变成 /index.html
			break;
		case HEADERS:					   // 状态2 解析头部
			ParseHeader_(line);			   // 处理头部
			if (buff.ReadableBytes() <= 2) // 如果只剩下 \r\n GET请求到此结束
			{
				state_ = FINISH;
			}
			break;
		case BODY:			  // 解析正文
			ParseBody_(line); // 处理请求体
			break;
		default:
			break;
		}
		// 这里就是说可读数据搜索完都没发现 \r\n
		//  如果缓冲区的数据不足构成一行，退出循环，等待更多数据
		if (lineEnd == buff.BeginWrite())
		{
			break;
		}
		buff.RetrieveUntil(lineEnd + 2); // 跳过回车换行
	}
	LOG_DEBUG("[%s], [%s], [%s]", method_.c_str(), path_.c_str(), version_.c_str());
	return true;
}

// 解析路径（规范化请求的path_）
void HttpRequest::ParsePath_()
{
	if (path_ == "/")
	{
		path_ = "/index.html";
	}
	else
	{
		for (auto &item : DEFAULT_HTML)
		{
			if (item == path_)
			{
				path_ += ".html";
				break;
			}
		}
	}
}

// 解析请求行 例如 GET /index.html HTTP/1.1
bool HttpRequest::ParseRequestLine_(const string &line)
{
	regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$"); // 定义一个正则表达式
	smatch subMatch;
	// 在匹配规则中，以括号()的方式来划分组别 一共三个括号 [0]表示整体
	if (regex_match(line, subMatch, patten)) // 用pattern去匹配 line的格式
	{										 // 匹配指定字符串整体是否符合
		// 若格式合格，开始捕获
		method_ = subMatch[1];	// 第一个（）内是method_
		path_ = subMatch[2];	// 第二个（）内是path_
		version_ = subMatch[3]; // 第三个（）内是version_
		state_ = HEADERS;		// 状态转换为下一个状态
		return true;
	}
	LOG_ERROR("RequestLine Error");
	return false;
}

// 解析头部
void HttpRequest::ParseHeader_(const string &line)
{
	regex patten("^([^:]*): ?(.*)$");
	smatch subMatch;
	if (regex_match(line, subMatch, patten))
	{
		header_[subMatch[1]] = subMatch[2]; // 若格式匹配成功，将键 值对存入header_ map中
	}
	else // 如果解析失败，则意味着遇到了一个空行，标志着头部结束
	{
		state_ = BODY; // 状态转换为下一个状态
	}
}

// 解析请求体
void HttpRequest::ParseBody_(const string &line)
{
	body_ = line;
	ParsePost_();	 // 尝试将body_解析为POST表单数据
	state_ = FINISH; // 状态转换为下一个状态
	LOG_DEBUG("Body:%s, len:%d", line.c_str(), line.size());
}

// 16进制转化为10进制
int HttpRequest::ConverHex(char ch) // 将16进制字符（'0'-'9','a'-'f','A'-'F'）转换成对应的0-15的整数
{
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	return ch - '0';
}

// 处理post请求
void HttpRequest::ParsePost_()
{
	if (method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded")
	{
		ParseFromUrlencoded_();			   // 用ParseFromUrlencoded_来解析body_并填充post_ map
		if (DEFAULT_HTML_TAG.count(path_)) // 检查当前请求的path_是否在DEFAULT_HTML_TAG中
		{								   // 如果是登录/注册的path
			int tag = DEFAULT_HTML_TAG.find(path_)->second;
			LOG_DEBUG("Tag:%d", tag);
			if (tag == 0 || tag == 1)
			{
				bool isLogin = (tag == 1); // 为1则是登录
				if (UserVerify(post_["username"], post_["password"], isLogin))
				{
					path_ = "/welcome.html";
				}
				else
				{
					path_ = "/error.html";
				}
			}
		}
	}
}

// 从body_中解码所有application/x-www-form-urlencoded格式的数据（即key1=value1&key2=value2）
void HttpRequest::ParseFromUrlencoded_()
{
	if (body_.size() == 0)
	{
		return;
	}

	string key, value;
	int num = 0;
	int n = body_.size();
	int i = 0, j = 0;

	for (; i < n; i++)
	{
		char ch = body_[i];
		switch (ch)
		{
			// key
		case '=':
			key = body_.substr(j, i - j);
			j = i + 1;
			break;
			// 键值对中的空格换为+或者%20
		case '+':
			body_[i] = ' ';
			break;
		case '%':
			num = ConverHex(body_[i + 1]) * 16 + ConverHex(body_[i + 2]);
			body_[i + 2] = num % 10 + '0';
			body_[i + 1] = num / 10 + '0';
			i += 2;
			break;
			// 键值对连接符
		case '&':
			value = body_.substr(j, i - j);
			j = i + 1;
			post_[key] = value;
			LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
			break;
		default:
			break;
		}
	}
	assert(j <= i);
	if (post_.count(key) == 0 && j < i)
	{
		value = body_.substr(j, i - j);
		post_[key] = value;
	}
}

// 执行数据库的用户登陆 和 注册 操作
bool HttpRequest::UserVerify(const string &name, const string &pwd, bool isLogin)
{
	// 验证用户名或者密码是否为空
	if (name == "" || pwd == "")
	{
		return false;
	}
	LOG_INFO("Verify name:%s pwd:******", name.c_str());

	// 声明一个原始的mysql连接指针
	MYSQL *sql;
	SqlConnRAII(&sql, SqlConnPool::Instance());
	assert(sql);

	// 初始化变量
	bool flag = false;
	unsigned int j = 0;
	char order[256] = {0};
	MYSQL_FIELD *fields = nullptr;
	MYSQL_RES *res = nullptr;

	if (!isLogin)
	{
		flag = true;
	}
	/* 查询用户及密码 */
	// snprintf拼接sql查询字符串
    snprintf(order, 256, "SELECT username, passwd FROM user WHERE username='%s' LIMIT 1", name.c_str());
	LOG_DEBUG("%s", order);

	// 若返回非0值则查询失败
	if (mysql_query(sql, order))
	{
		return false; // 查询失败
	}

	res = mysql_store_result(sql); // 若成功，存储查询结果

	j = mysql_num_fields(res);
	fields = mysql_fetch_fields(res);

	while (MYSQL_ROW row = mysql_fetch_row(res))
	{
		LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
		string password(row[1]);

		// 若是登陆，判断传入的pwd和数据库中存储的password是否一致
		if (isLogin)
		{
			if (pwd == password)
			{
				flag = true;
			}
			else
			{
				flag = false;
				LOG_INFO("pwd error!");
			}
		}
		// 若是注册，则表明此用户已经存在
		else
		{
			flag = false;
			LOG_INFO("user used!");
		}
	}
	// 释放结果集
	mysql_free_result(res);

	/* 注册行为 且 用户名未被使用*/
	if (!isLogin && flag == true)
	{
		LOG_DEBUG("regirster!");
		bzero(order, 256);
        snprintf(order, 256, "INSERT INTO user(username, passwd) VALUES('%s','%s')", name.c_str(), pwd.c_str());
		LOG_DEBUG("%s", order);
		if (mysql_query(sql, order))
		{
			LOG_DEBUG("Insert error!");
			flag = false;
		}
		else
		{
			flag = true;
		}
	}
	LOG_DEBUG("UserVerify success!!");
	return flag;
}

string HttpRequest::path() const
{
	return path_;
}

string &HttpRequest::path()
{
	return path_;
}
string HttpRequest::method() const
{
	return method_;
}

string HttpRequest::version() const
{
	return version_;
}

string HttpRequest::GetPost(const string &key) const
{
	assert(key != "");
	if (post_.count(key) == 1)
	{
		return post_.find(key)->second;
	}
	return "";
}

string HttpRequest::GetPost(const char *key) const
{
	assert(key != nullptr);
	if (post_.count(key) == 1)
	{
		return post_.find(key)->second;
	}
	return "";
}
