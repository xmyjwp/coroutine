#include "hook.h"
#include "ioscheduler.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.h"
#include <string.h>

// apply XX to all functions
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 

namespace sylar{

//定义一个全局变量，用于记录当前线程是否使用了hooked函数
//当 t_hook_enable 为 true 时，表示启用HOOK功能，程序会拦截特定的函数调用（如 sleep、read、write 等），并将其转换为异步操作。
//当 t_hook_enable 为 false 时，表示禁用HOOK功能，程序会直接调用原始的函数，不进行任何拦截或修改。
static thread_local bool t_hook_enable = false;

bool is_hook_enable()
{
    return t_hook_enable;
}

void set_hook_enable(bool flag)
{
    t_hook_enable = flag;
}

void hook_init()
{
	static bool is_inited = false;
	if(is_inited)
	{
		return;
	}

	// test
	is_inited = true;

//通过 dlsym 函数获取系统库中的原始函数地址，并将其赋值给相应的函数指针，以便在后续代码中能够调用这些原始函数
// assignment -> sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
	HOOK_FUN(XX)
#undef XX
}

// static variable initialisation will run before the main function
struct HookIniter
{
	HookIniter()
	{
		hook_init();
	}
};

// hook_init() 放在⼀个静态对象的构造函数中调⽤，这表示在main函数运⾏之前就会获取各个符号的地址并保存在全局变量中。
static HookIniter s_hook_initer;

} // end namespace sylar


struct timer_info 
{
    int cancelled = 0;
};


// Hook 机制的核心逻辑封装​​，它通过模板化设计统一处理所有读/写类系统调用（如 read, write, recv, send 等），
// 实现了 ​​非阻塞操作、超时管理和协程调度​​ 的透明化
// universal template for read and write function
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name, uint32_t event, int timeout_so, Args&&... args) 
{
    if(!sylar::t_hook_enable) 
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(!ctx) 
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    if(ctx->isClosed()) 
    {
        errno = EBADF;  //错误码：文件描述符无效
        return -1;
    }

    // 非 Socket 或用户显式设置非阻塞​​：直接调用原始函数，不进行协程调度
    if(!ctx->isSocket() || ctx->getUserNonblock()) 
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 从 FdCtx 中获取预先设置的读/写超时时间（毫秒）并创建定时器信息 tinfo，用于后续超时回调
    // get the timeout
    uint64_t timeout = ctx->getTimeout(timeout_so);  // timeout_so是一个套接字选项：接收超时时间或发送超时时间
    // timer condition
    std::shared_ptr<timer_info> tinfo(new timer_info);

retry:
	// run the function
    ssize_t n = fun(fd, std::forward<Args>(args)...);
    
    // EINTR ->Operation interrupted by system ->retry
    while(n == -1 && errno == EINTR)   //错误码EINTR：操作被系统中断，不断重试
    {
        n = fun(fd, std::forward<Args>(args)...);
    }
    
    // 0 resource was temporarily unavailable -> retry until ready 
    if(n == -1 && errno == EAGAIN)  //EAGAIN：资源暂时不可用（数据未就绪），挂起协程并等待事件
    {
        sylar::IOManager* iom = sylar::IOManager::GetThis();
        // timer
        std::shared_ptr<sylar::Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        // 1 timeout has been set -> add a conditional timer for canceling this operation
        if(timeout != (uint64_t)-1) 
        {
            //获取当前的 I/O 管理器 iom，并设置一个条件定时器 timer，用于在超时后取消事件
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]() 
            {
                auto t = winfo.lock(); //尝试将weak_ptr转换为shared_ptr
                if(!t || t->cancelled) 
                {
                    return;
                }
                t->cancelled = ETIMEDOUT;  //连接超时
                // 取消这个事件并触发一次，以便返回到这个协程
                iom->cancelEvent(fd, (sylar::IOManager::Event)(event));
            }, winfo);
        }

        // 将 FD 的读/写事件注册到 epoll
        // 2 add event -> callback is this fiber
        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        if(rt)  // 若注册失败（rt != 0），输出错误日志并取消定时器
        {
            std::cout << hook_fun_name << " addEvent("<< fd << ", " << event << ")";
            if(timer) 
            {
                timer->cancel();
            }
            return -1;
        } 
        else 
        {
            sylar::Fiber::GetThis()->yield();  // 当前协程主动让出 CPU，等待事件就绪或超时触发
     
            // 3 resume either by addEvent or cancelEvent
            if(timer)  // 若事件提前就绪，取消未触发的定时器
            {
                timer->cancel();
            }
            // by cancelEvent
            if(tinfo->cancelled == ETIMEDOUT)  // 若因超时被唤醒，返回 ETIMEDOUT 错误
            {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }
    return n;
}



extern "C"{

// declaration -> sleep_fun sleep_f = nullptr;
#define XX(name) name ## _fun name ## _f = nullptr;
	HOOK_FUN(XX)
#undef XX

// only use at task fiber
unsigned int sleep(unsigned int seconds)
{
	if(!sylar::t_hook_enable)
	{
		return sleep_f(seconds);
	}

	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	sylar::IOManager* iom = sylar::IOManager::GetThis();
	// add a timer to reschedule this fiber
	iom->addTimer(seconds*1000, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	// wait for the next resume
	fiber->yield();
	return 0;
}

int usleep(useconds_t usec)
{
	if(!sylar::t_hook_enable)
	{
		return usleep_f(usec);
	}

	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	sylar::IOManager* iom = sylar::IOManager::GetThis();
	// add a timer to reschedule this fiber
	iom->addTimer(usec/1000, [fiber, iom](){iom->scheduleLock(fiber);});
	// wait for the next resume
	fiber->yield();
	return 0;
}

// 实现 秒 和 纳秒 级睡眠
int nanosleep(const struct timespec* req, struct timespec* rem)
{
	if(!sylar::t_hook_enable)
	{
		return nanosleep_f(req, rem);
	}	

	int timeout_ms = req->tv_sec*1000 + req->tv_nsec/1000/1000;

	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	sylar::IOManager* iom = sylar::IOManager::GetThis();
	// add a timer to reschedule this fiber
	iom->addTimer(timeout_ms, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	// wait for the next resume
	fiber->yield();	
	return 0;
}

int socket(int domain, int type, int protocol)
{
	if(!sylar::t_hook_enable)
	{
		return socket_f(domain, type, protocol);
	}	

	int fd = socket_f(domain, type, protocol);
	if(fd==-1)
	{
		std::cerr << "socket() failed:" << strerror(errno) << std::endl;
		return fd;
	}
	sylar::FdMgr::GetInstance()->get(fd, true);  // 加入文件描述符管理器
	return fd;
}

int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) 
{
    if(!sylar::t_hook_enable) 
    {
        return connect_f(fd, addr, addrlen);
    }

    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(!ctx || ctx->isClosed()) 
    {
        errno = EBADF;
        return -1;
    }

    if(!ctx->isSocket())  // 若 fd 不是 Socket（如普通文件），直接调用原始 connect
    {
        return connect_f(fd, addr, addrlen);
    }

    if(ctx->getUserNonblock())  // 若用户通过 fcntl 显式设置了非阻塞模式，直接调用原始 connect，协程库不介入
    {
        return connect_f(fd, addr, addrlen);
    }

    // attempt to connect
    int n = connect_f(fd, addr, addrlen);
    if(n == 0) 
    {
        return 0;  //  立即成功
    } 
    else if(n != -1 || errno != EINPROGRESS) // 没有发生“操作正在进行中”的错误
    {
        return n;
    }

    // wait for write event is ready -> connect succeeds
    sylar::IOManager* iom = sylar::IOManager::GetThis();
    std::shared_ptr<sylar::Timer> timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    if(timeout_ms != (uint64_t)-1) 
    {
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]() 
        {
            auto t = winfo.lock();
            if(!t || t->cancelled) 
            {
                return;
            }
            t->cancelled = ETIMEDOUT;
            iom->cancelEvent(fd, sylar::IOManager::WRITE);
        }, winfo);
    }

    int rt = iom->addEvent(fd, sylar::IOManager::WRITE);
    if(rt == 0) // 表示添加操作成功或至少没有立即失败
    {
        sylar::Fiber::GetThis()->yield();

        // resume either by addEvent or cancelEvent
        if(timer) 
        {
            timer->cancel();
        }

        if(tinfo->cancelled) 
        {
            errno = tinfo->cancelled;
            return -1;
        }
    } 
    else 
    {
        if(timer) 
        {
            timer->cancel();
        }
        std::cerr << "connect addEvent(" << fd << ", WRITE) error";
    }

    // check out if the connection socket established 
    int error = 0;
    socklen_t len = sizeof(int);
    if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) // 用于检查套接字 fd 的错误状态
    {
        return -1;
    }
    if(!error) 
    {
        return 0;
    } 
    else 
    {
        errno = error;
        return -1;
    }
}


static uint64_t s_connect_timeout = -1;
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	int fd = do_io(sockfd, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);	
	if(fd>=0)
	{
		sylar::FdMgr::GetInstance()->get(fd, true);
	}
	return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
	return do_io(fd, read_f, "read", sylar::IOManager::READ, SO_RCVTIMEO, buf, count);	
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, readv_f, "readv", sylar::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);	
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
	return do_io(sockfd, recv_f, "recv", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags);	
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
	return do_io(sockfd, recvfrom_f, "recvfrom", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);	
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	return do_io(sockfd, recvmsg_f, "recvmsg", sylar::IOManager::READ, SO_RCVTIMEO, msg, flags);	
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return do_io(fd, write_f, "write", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, count);	
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, writev_f, "writev", sylar::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);	
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
	return do_io(sockfd, send_f, "send", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);	
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
	return do_io(sockfd, sendto_f, "sendto", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);	
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
	return do_io(sockfd, sendmsg_f, "sendmsg", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, flags);	
}

int close(int fd)
{
	if(!sylar::t_hook_enable)
	{
		return close_f(fd);
	}	
     // 1. 获取上下文并标记为已关闭
	std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

	if(ctx)
	{
		auto iom = sylar::IOManager::GetThis();
		if(iom)
		{	// 2. 取消该 FD 的所有 IO 事件监听
			iom->cancelAll(fd);
		}
		// del fdctx
		sylar::FdMgr::GetInstance()->del(fd);
	}
	return close_f(fd);
}

int fcntl(int fd, int cmd, ... /* arg */ )
{
  	va_list va; // 定义 va_list 变量

    va_start(va, cmd);  // 初始化 va，使其指向可变参数列表的起始位置（即第三个参数）
    switch(cmd) 
    {
        case F_SETFL:     //设置文件描述符的标志
            {
                int arg = va_arg(va, int); // 提取下一个参数（类型为 int）
                va_end(va);                         // 结束对可变参数的访问
                std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);   //获取文件描述符对应的上下文FdCtx
                if(!ctx || ctx->isClosed() || !ctx->isSocket())   
                {
                    return fcntl_f(fd, cmd, arg);  // 非 Socket 直接透传
                }
                // 用户是否设定了非阻塞
                ctx->setUserNonblock(arg & O_NONBLOCK);
                // 最后是否阻塞根据系统设置决定
                if(ctx->getSysNonblock()) 
                {
                    arg |= O_NONBLOCK;
                } 
                else 
                {
                    arg &= ~O_NONBLOCK;
                }
                // ​​分离用户层与系统层状态​​：用户设置的 O_NONBLOCK 被记录到 UserNonblock，
                //但实际传递给系统的标志由协程库控制（强制非阻塞）。
                //​示例​​：用户调用 fcntl(F_SETFL, 0)（设为阻塞），但底层仍为非阻塞，协程通过挂起模拟阻塞
                return fcntl_f(fd, cmd, arg);
            }
            break;

        case F_GETFL:     //获取文件描述符的标志
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return arg;
                }
                //​透明性保证​​：返回用户设置的 O_NONBLOCK 状态，而非实际系统标志，避免用户感知底层非阻塞。
                 //​示例​​：底层实际为非阻塞，但用户调用 fcntl(F_GETFL) 可能看到阻塞标志，协程库通过挂起模拟阻塞行为
                if(ctx->getUserNonblock()) 
                {
                    return arg | O_NONBLOCK;
                } else 
                {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;

        // 文件描述符复制、关闭标志设置等命令无需协程库干预，直接调用原始函数
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg); 
            }
            break;


        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;

        // 处理文件锁
        case F_SETLK:           
        case F_SETLKW:
        case F_GETLK:
            {
                struct flock* arg = va_arg(va, struct flock*);  //获取可变参数中的文件锁结构体指针
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

//处理扩展的文件所有者信息
        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }	
}

// 用户设置的非阻塞状态（UserNonblock）仅被记录到 FdCtx，
// 实际底层非阻塞状态由协程库强制控制（SysNonblock），因此仍需调用原始 ioctl 设置系统标志。
int ioctl(int fd, unsigned long request, ...)
{
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    if(FIONBIO == request)  // 检查是否为设置非阻塞标志的请求
    {
        bool user_nonblock = !!*(int*)arg;  //!! 操作符用于将值转换为布尔类型（true 或 false），表示是否启用非阻塞模式
        std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
        if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
        {
            return ioctl_f(fd, request, arg);
        }
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(fd, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
	return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    if(!sylar::t_hook_enable) 
    {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    if(level == SOL_SOCKET)    //套接字级别
    {
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)  // 接收操作超时时间 或 发送操作超时时间
        {
            std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(sockfd);
            if(ctx) 
            {
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);	
}

}
