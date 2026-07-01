#include "buffer.h"
using namespace std;

buffer::buffer(int initbuffersize) : buffer_(initbuffersize), readPos_(0), writePos_(0)
{
    // 初始化大小为initbuffersize的vector<char>
    // 初始的读写指针都在0的位置，表示缓冲区为空
}

// 计算剩余可写空间
size_t buffer::WritableBytes() const
{
    return buffer_.size() - writePos_;
}

// 计算可读字节数
size_t buffer::ReadableBytes() const
{
    return writePos_ - readPos_;
}

// 计算头部空间空间，读指针之前的所有空间都是已经被读取过的
size_t buffer::PrependableBytes() const
{
    return readPos_;
}

// 返回当前有效数据的起始地址
const char *buffer::Peek() const
{
    return &buffer_[readPos_];
}

// 确保当前可写空间是否足够容纳len字节，如果不够，使用MakeSpace_扩容
void buffer::EnsureWriteable(size_t len)
{
    if (len > WritableBytes())
    {
        MakeSpace_(len);
    }
    assert(len <= WritableBytes()); // 断言检查，验证len <= WritableBytes()，若为真 程序继续执行，若为假 程序异常退出
}

// 写入数据后，更新写指针writePos_，增加len
void buffer::HasWriten(size_t len)
{
    writePos_ += len;
}

// 读取len字节的数据，并更新读指针readPos_
void buffer::Retrieve(size_t len)
{
    readPos_ += len;
}

// 读取数据直到end
void buffer::RetrieveUntil(const char *end)
{
    assert(Peek() <= end);
    Retrieve(end - Peek());
}

// 读取所有数据，重置指针
void buffer::RetrieveAll()
{
    bzero(&buffer_[0], buffer_.size()); // 覆盖原本数据
    readPos_ = writePos_ = 0;
}

// 读取所有数据，并以string形式返回
string buffer::RetrieveAllToStr()
{
    string str(Peek(), ReadableBytes()); // 用当前可读的数据构造一个string
    RetrieveAll();                       // 清空缓冲区
    return str;                          // 返回取出的数据
}

// 返回当前可写位置的常量指针(只读)，这个的作用是告诉你如果能写，要从哪里开始写，但不能真的去写
const char *buffer::BeginWriteConst() const
{
    return &buffer_[writePos_];
}

// 回当前可写位置的指针，允许直接在返回的地址直接写入数据
char *buffer::BeginWrite()
{
    return &buffer_[writePos_];
}

// 多个重载形式，用于向缓冲区追加数据，包括string，c风格字符串,通用二进制数据（void*）,buffer对象数据
void buffer::Append(const char *str, size_t len)
{
    assert(str);
    EnsureWriteable(len);
    // cout << "dubug:start len=" << len << "writepos=" << writePos_ << endl;
    copy(str, str + len, BeginWrite()); // 将数据拷贝到当前可写的位置
    HasWriten(len);
    // cout << "writepos=" << writePos_ << endl;
}
void buffer::Append(const string &str)
{
    Append(str.c_str(), str.size());
}
void buffer::Append(const void *data, size_t len)
{
    Append(static_cast<const char *>(data), len); // static_cast<const char*> (data) 将data强制转换成const char*
}
void buffer::Append(const buffer &buff)
{
    Append(buff.Peek(), buff.ReadableBytes()); // 从另一个buffer对象中拷贝数据
}

// 从文件描述符fd读取数据到缓冲区
ssize_t buffer::ReadFd(int fd, int *Errno)
{
    char buff[65535];                   // 在栈区分配一个64kb的临时缓冲区
    struct iovec iov[2];                // iovec结构体，base为指针，len为长度
    size_t writeable = WritableBytes(); // 先记录能写多少
    // 分散读数据，保证数据全部读完
    // iov[0]指向buffer内部当前剩余可写空间
    // iov[1]指向栈上的临时缓冲区
    iov[0].iov_base = BeginWrite();
    iov[0].iov_len = writeable;
    iov[1].iov_base = buff;
    iov[1].iov_len = sizeof(buff);

    ssize_t len = readv(fd, iov, 2);
    if (len < 0)
    {
        *Errno = errno;
    }
    else if (static_cast<size_t>(len) <= writeable) // 这里如果成立，则说明写区可容纳len
    {
        writePos_ += len;
    }
    else
    {
        writePos_ = buffer_.size(); // 写区写满了，下标移动到最后
        Append(buff, static_cast<size_t>(len - writeable));
        // 超出buffer当前可写区的数据会先存在临时区间buff内
        // 然后将缓存在buff内的数据copy到buffer缓冲区中，这期间buffer会通过EnsureWriteable接口进行扩容
    }
    return len;
}

// 将缓冲区的数据写入到文件描述符fd
ssize_t buffer::WriteFd(int fd, int *Errno)
{
    ssize_t len = write(fd, Peek(), ReadableBytes());
    if (len < 0)
    {
        *Errno = errno;
    }
    Retrieve(len); // 不一定一次性能发完所有数据（非阻塞模式下），用Retrieve(len)更新读指针
    return len;
}

// 返回底层缓冲区的起始地址
char *buffer::BeginPtr_() // （可写）
{
    return &buffer_[0];
}
const char *buffer::BeginPtr_() const // （只读）
{
    return &buffer_[0];
}
void buffer::MakeSpace_(size_t len) // 在空间不足时扩展缓冲区或整理碎片空间
{
    if (WritableBytes() + PrependableBytes() < len)
    {
        buffer_.resize(writePos_ + len + 1);
    }
    else // 若尾部空间不够，不需要扩容，直接将数据搬移直头部空间
    {
        size_t readable = ReadableBytes();
        copy(BeginPtr_() + readPos_, BeginPtr_() + writePos_, BeginPtr_());
        readPos_ = 0;
        writePos_ = readable;
        assert(readable == ReadableBytes());
    }
}
