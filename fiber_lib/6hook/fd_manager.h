#ifndef _FD_MANAGER_H_
#define _FD_MANAGER_H_

#include <memory>
#include <shared_mutex>
#include <vector>
#include "thread.h"


namespace sylar{

// fd info
class FdCtx : public std::enable_shared_from_this<FdCtx>
{
private:
	// 是否初始化
	bool m_isInit = false;
	// 是否是socket
	bool m_isSocket = false;
	//是否被系统设置为非阻塞（系统调用）
	bool m_sysNonblock = false;
	//是否被用户设置为非阻塞（用户调用）
	bool m_userNonblock = false;
	//是否关闭
	bool m_isClosed = false;
	//文件描述符
	int m_fd;

	// read event timeout
	uint64_t m_recvTimeout = (uint64_t)-1;
	// write event timeout
	uint64_t m_sendTimeout = (uint64_t)-1;

public:
	FdCtx(int fd);
	~FdCtx();

	bool init();
	bool isInit() const {return m_isInit;}
	bool isSocket() const {return m_isSocket;}
	bool isClosed() const {return m_isClosed;}

	void setUserNonblock(bool v) {m_userNonblock = v;}
	bool getUserNonblock() const {return m_userNonblock;}

	void setSysNonblock(bool v) {m_sysNonblock = v;}
	bool getSysNonblock() const {return m_sysNonblock;}

	void setTimeout(int type, uint64_t v);  //设置超时时间，type=SO_RCVTIMEO表示读超时，type=SO_SNDTIMEO表示写超时
	uint64_t getTimeout(int type);
};

class FdManager
{
public:
	FdManager();

	std::shared_ptr<FdCtx> get(int fd, bool auto_create = false); // 获取或创建 FD 上下文
	void del(int fd);    // 删除 FD 上下文（close 时调用）

private:
	std::shared_mutex m_mutex;
	std::vector<std::shared_ptr<FdCtx>> m_datas;
};


template<typename T>
class Singleton
{
private:
    static T* instance;
    static std::mutex mutex;

protected:
    Singleton() {}  

public:
    // Delete copy constructor and assignment operation
	//这两行代码确保了Singleton类的实例无法被拷贝或赋值，从而保证了单例模式的正确实现
    Singleton(const Singleton&) = delete;  //删除类的拷贝构造函数
    Singleton& operator=(const Singleton&) = delete;  //删除类的赋值运算符

    static T* GetInstance() 
    {
//std::lock_guard是C++11中提供的一个模板类，它可以用来对互斥量进行加锁和解锁，以确保线程安全
        std::lock_guard<std::mutex> lock(mutex); // Ensure thread safety
        if (instance == nullptr) 
        {
            instance = new T();
        }
        return instance;
    }

    static void DestroyInstance() 
    {
        std::lock_guard<std::mutex> lock(mutex);
        delete instance;
        instance = nullptr;
    }
};

typedef Singleton<FdManager> FdMgr;

}

#endif