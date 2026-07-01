#pragma
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <regex> //正则表达式
#include <errno.h>
#include <mysql/mysql.h>
#include "sqlconnpool.h"
using namespace std;

class HttpRequest
{
public:
    enum PARSE_STATE // 枚举类型.定义了有限状态机的所有可能状态
    {
        REQUEST_LINE, // 状态1，解析请求行
        HEADERS,      // 状态2，解析头部字段
        BODY,         // 状态3，解析请求体
        FINISH,       // 状态4，解析完成
    };

    HttpRequest() { Init(); }
    ~HttpRequest() = default;

    void Init();
    bool parse(buffer &buff); // 解析函数，从buff中读取数据，并根据当前的state_来逐行解析

    string path() const;    // 只读访问路径
    string &path();         // 允许服务器逻辑修改路径
    string method() const;  // 返回请求方法
    string version() const; // 返回http版本

    string GetPost(const string &key) const; // 从post_ map中获取post表单数据
    string GetPost(const char *key) const;

    bool IsKeepAlive() const; // 用于判断是否应保持TCP连接

private:
    bool ParseRequestLine_(const string &line); // 处理请求行
    void ParseHeader_(const string &line);      // 处理请求头
    void ParseBody_(const string &line);        // 处理请求体
    void ParsePath_();                          // 处理请求路径
    void ParsePost_();                          // 处理post事件
    void ParseFromUrlencoded_();                // 从url中解析编码

    static bool UserVerify(const string &name, const string &pwd, bool isLogin); // 用户验证

    static int ConverHex(char ch); // 16进制转10进制

    PARSE_STATE state_;                     // 用来存储有限状态机的状态
    string method_, path_, version_, body_; // 存储从请求中解析出的核心数据
    unordered_map<string, string> header_;  // 存储所有HTTP头部
    unordered_map<string, string> post_;    // 存储所有POST表单数据

    static const unordered_set<string> DEFAULT_HTML;
    static const unordered_map<string, int> DEFAULT_HTML_TAG;
};