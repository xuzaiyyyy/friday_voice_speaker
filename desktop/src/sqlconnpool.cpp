#include "sqlconnpool.h"
using namespace std;

SqlConnPool *SqlConnPool::Instance()
{
    static SqlConnPool pool;
    return &pool;
}

void SqlConnPool::Init(const char *host, int port, const char *user,
                       const char *pwd, const char *dbName, int connSize = 10)
{
    assert(connSize > 0);
    for (int i = 0; i < connSize; i++)
    {
        MYSQL *conn = nullptr;
        conn = mysql_init(conn); // 初始化一个MYSQL对象
        if (!conn)
        {
            // fprintf(stderr, "error:mysql_init failed");
            LOG_ERROR("Mysql init error!");
            assert(conn);
        }
        conn = mysql_real_connect(conn, host, user, pwd, dbName, port, nullptr, 0); // 尝试与Mysql建立物理连接
        if (!conn)
        {
            // fprintf(stderr, "error:mysql connect failed");
            LOG_ERROR("Mysql Connect error!");

            continue;
        }
        connQue_.emplace(conn);
        // fprintf(stderr, "success %d\n", i);
    }
    MAX_CONN_ = connSize;
    sem_init(&semId_, 0, MAX_CONN_); // 初始化信号量，第二参数0表示该信号量在当前进程的线程间共享
    // MAX_CONN_表示信号量的初始值，有多少个可用连接
}

MYSQL *SqlConnPool::GetConn()
{
    MYSQL *conn = nullptr;
    if (connQue_.empty())
    {
        LOG_WARN("SqlConnPool busy!");
        return nullptr;
    }
    sem_wait(&semId_);
    lock_guard<mutex> locker(mtx_);
    conn = connQue_.front();
    connQue_.pop();
    return conn;
}

void SqlConnPool::FreeConn(MYSQL *conn)
{
    assert(conn);
    lock_guard<mutex> locker(mtx_);
    connQue_.push(conn);
    sem_post(&semId_);
}

void SqlConnPool::ClosePool()
{
    lock_guard<mutex> locker(mtx_);
    while (!connQue_.empty()) // 循环直到所有空闲连接都被处理完毕
    {
        auto conn = connQue_.front();
        connQue_.pop();
        mysql_close(conn); // 关闭数据库连接
    }
    mysql_library_end(); // 释放mysql c客户端库所占用的全局资源
}

// 返回当前队列中的空闲连接数
int SqlConnPool::GetFreeConnCount()
{
    lock_guard<mutex> locker(mtx_);
    return connQue_.size();
}
