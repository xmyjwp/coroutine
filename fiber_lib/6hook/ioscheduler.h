#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "scheduler.h"
#include "timer.h"

namespace sylar {

// work flow
// 1 register one event -> 2 wait for it to ready -> 3 schedule the callback -> 4 unregister the event -> 5 run the callback
class IOManager : public Scheduler, public TimerManager 
{
public:
    enum Event    //是一个位掩码，可以同时表示多个事件
    {
        NONE = 0x0,
        // READ == EPOLLIN
        READ = 0x1,
        // WRITE == EPOLLOUT
        WRITE = 0x4
    };

private:
    // fd context  文件描述符上下文
    struct FdContext 
    {
        // event context   事件上下文
        struct EventContext 
        {
            // scheduler     调度器
            Scheduler *scheduler = nullptr;
            // callback fiber  协程
            std::shared_ptr<Fiber> fiber;
            // callback function    回调函数
            std::function<void()> cb;
        };

        // read event context
        EventContext read; 
        // write event context
        EventContext write;
        int fd = 0;
        // events registered
        Event events = NONE;
        std::mutex mutex;

        EventContext& getEventContext(Event event);
        void resetEventContext(EventContext &ctx);
        void triggerEvent(Event event); 
    };

public:
    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
    ~IOManager();

    // add one event at a time
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // delete event
    bool delEvent(int fd, Event event);
    // delete the event and trigger its callback
    bool cancelEvent(int fd, Event event);
    // delete all events and trigger its callback
    bool cancelAll(int fd);
    //获取正在运行的IOManager实例
    static IOManager* GetThis();

protected:
    void tickle() override;     //override 表明重写基类中的方法
    
    bool stopping() override;
    
    void idle() override;

    void onTimerInsertedAtFront() override;

    void contextResize(size_t size);

private:
    //epoll实例的文件描述符
    int m_epfd = 0;
    // 管道 fd[0] read，fd[1] write
    int m_tickleFds[2];
    // 原子变量，记录当前待处理的事件数量
    std::atomic<size_t> m_pendingEventCount = {0};
    //共享互斥锁，读共享锁，写互斥锁
    std::shared_mutex m_mutex;
    // socket事件上下⽂的容器 与文件描述符相关的上下文信息
    std::vector<FdContext *> m_fdContexts;
};

} // end namespace sylar

#endif