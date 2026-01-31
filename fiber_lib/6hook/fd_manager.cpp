#include "fd_manager.h"
#include "hook.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sylar{

// instantiate
template class Singleton<FdManager>;

// Static variables need to be defined outside the class
template<typename T>
T* Singleton<T>::instance = nullptr;

template<typename T>
std::mutex Singleton<T>::mutex;	   //模板类的静态成员变量需要在类外进行显式定义和初始化

FdCtx::FdCtx(int fd):
m_fd(fd)
{
	init();
}

FdCtx::~FdCtx()
{

}

bool FdCtx::init()
{
	if(m_isInit)
	{
		return true;
	}
	
	struct stat statbuf;   //声明一个stat结构体，用于存储文件状态信息
	// 使用fstat系统调用获取文件描述符的状态信息
	if(-1==fstat(m_fd, &statbuf)) // fd is in valid
	{
		m_isInit = false;
		m_isSocket = false;
	}
	else
	{
		m_isInit = true;	
		m_isSocket = S_ISSOCK(statbuf.st_mode);	   // 使用S_ISSOCK宏检查文件描述符是否为socket
	}

	// if it is a socket -> set to nonblock
	if(m_isSocket)
	{
		// fcntl_f() -> the original fcntl() -> get the socket info
		int flags = fcntl_f(m_fd, F_GETFL, 0);  // 使用fcntl_f函数获取当前文件描述符的标志
		if(!(flags & O_NONBLOCK))
		{
			// if not -> set to nonblock
			fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK);
		}
		m_sysNonblock = true;
	}
	else
	{
		m_sysNonblock = false;
	}

	return m_isInit;
}

void FdCtx::setTimeout(int type, uint64_t v)
{
	if(type==SO_RCVTIMEO)
	{
		m_recvTimeout = v;
	}
	else
	{
		m_sendTimeout = v;
	}
}

uint64_t FdCtx::getTimeout(int type)
{
	if(type==SO_RCVTIMEO)  //SO_RCVTIMEO是一个套接字选项：接收超时时间
	{
		return m_recvTimeout;
	}
	else
	{
		return m_sendTimeout;
	}
}

FdManager::FdManager()
{
	m_datas.resize(64);
}

// 获取或创建一个文件描述符对应的FdCtx对象，并确保在多线程环境下的线程安全性。
std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create) // auto_create指示如果 FdCtx 对象不存在时是否自动创建
{
	if(fd==-1)
	{
		return nullptr;
	}

	std::shared_lock<std::shared_mutex> read_lock(m_mutex);  //共享读锁
	if(m_datas.size() <= fd)
	{
		if(auto_create==false)
		{
			return nullptr;
		}
	}
	else
	{
		if(m_datas[fd]||!auto_create) // 如果 m_datas 容器中已经存在 fd 对应的 FdCtx 对象 或者 不需要自动创建
		{
			return m_datas[fd];
		}
	}

	read_lock.unlock();
	std::unique_lock<std::shared_mutex> write_lock(m_mutex);  //独占写锁

	if(m_datas.size() <= fd)
	{
		m_datas.resize(fd*1.5);
	}

	m_datas[fd] = std::make_shared<FdCtx>(fd);
	return m_datas[fd];

}

void FdManager::del(int fd)
{
	std::unique_lock<std::shared_mutex> write_lock(m_mutex);
	if(m_datas.size() <= fd)
	{
		return;
	}
	m_datas[fd].reset();
}

}