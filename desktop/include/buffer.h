#ifndef BUFFER_H
#define BUFFER_H

#include <cstring>
#include <iostream>
#include <unistd.h>
#include <sys/uio.h>
#include <vector>
#include <atomic>
#include <assert.h>
using namespace std;

// 定义一个buffer类，用来管理一个可自动扩展的内存缓冲区

class buffer
{
public:
    buffer(int initbuffersize = 1024); // 初始化环缓冲区，默认大小为1024字节
    ~buffer() = default;               // 使用默认的析构函数

    // const成员函数，只能读取，不能修改成员变量
    size_t WritableBytes() const;    // 返回缓冲区中当前可写入的字节数（剩余空间）
    size_t ReadableBytes() const;    // 返回缓冲区中当前可读取的字节数（已存数据量）
    size_t PrependableBytes() const; // 返回缓冲区头部预留的字节数（readPos_之前的空间）

    const char *Peek() const;         // 返回指向当前可读数据的起始位置的指针，允许用户查看数据但不移动读指针
    void EnsureWriteable(size_t len); // 确保缓冲区有足够的空间写入len字节的数据。如果空间不够，自动扩容
    void HasWriten(size_t len);       // 写入数据后，更新写指针writePos_，增加len

    void Retrieve(size_t len);           // 读取len字节的数据，并更新读指针readPos_
    void RetrieveUntil(const char *end); // 读取数据直到end

    void RetrieveAll();        // 读取所有数据，重置指针
    string RetrieveAllToStr(); // 读取所有数据，并以string形式返回

    // 这两个函数用处和用法不同
    const char *BeginWriteConst() const; // 返回当前可写位置的常量指针(只读)，这个的作用是告诉你如果能写，要从哪里开始写，但不能真的去写
    char *BeginWrite();                  // 回当前可写位置的指针，允许直接在返回的地址直接写入数据

    // 多个重载形式，用于向缓冲区追加数据，包括string，c风格字符串,通用二进制数据（void*）,buffer对象数据
    void Append(const string &str);
    void Append(const char *str, size_t len);
    void Append(const void *data, size_t len);
    void Append(const buffer &buff);

    ssize_t ReadFd(int fd, int *Errno);  // 从文件描述符fd读取数据到缓冲区
    ssize_t WriteFd(int fd, int *Errno); // 将缓冲区的数据写入到文件描述符fd

private:
    // 返回底层缓冲区的起始地址
    char *BeginPtr_();             // （可写）
    const char *BeginPtr_() const; // （只读）
    void MakeSpace_(size_t len);   // 在空间不足时扩展缓冲区或整理碎片空间

    vector<char> buffer_;     // 作为底层连续内存存储，用来存储数据
    atomic<size_t> readPos_;  // 读的下标
    atomic<size_t> writePos_; // 写的下标
};

#endif