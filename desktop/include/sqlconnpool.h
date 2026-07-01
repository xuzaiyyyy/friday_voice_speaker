#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H
#include "log.h"
#include <mysql/mysql.h>
#include <string>
#include <mutex>
#include <semaphore.h>
#include <thread>
#include <queue>
using namespace std;

class SqlConnPool
{
public:
    static SqlConnPool *Instance(); // 单例模式的标准访问函数，返回全局唯一的SqlConnPool实例

    MYSQL *GetConn();           // 获取连接
    void FreeConn(MYSQL *conn); // 归还连接
    int GetFreeConnCount();     // 返回当前连接池中可用的连接数

    void Init(const char *host, int port, const char *user,
              const char *pwd, const char *dbName, int connSize);

    void ClosePool();

private:
    SqlConnPool() = default;
    ~SqlConnPool() { ClosePool(); }

    int MAX_CONN_;
    queue<MYSQL *> connQue_; // 连接队列
    mutex mtx_;
    sem_t semId_; // 信号量。他是一个技术器，他的计数值表示connQue_中可用的连接数
    // GetConn会对信号量执行wait操作（计数-1）
    // FreeConn会对信号量执行post操作（计数+1）
};

class SqlConnRAII // 实现MYSQL连接的自动归还
{
public:
    SqlConnRAII(MYSQL **sql, SqlConnPool *connpool)
    {
        assert(connpool);
        *sql = connpool->GetConn();
        sql_ = *sql;
        connpool_ = connpool;
    }

    ~SqlConnRAII()
    {
        if (sql_)
        {
            connpool_->FreeConn(sql_); // 析构时自动归还连接
        }
    }

private:
    MYSQL *sql_;
    SqlConnPool *connpool_;
};

#endif