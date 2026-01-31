#include "scheduler.h"

static bool debug = false;

namespace sylar {

static thread_local Scheduler* t_scheduler = nullptr; //线程局变量，它负责管理线程池、任务队列以及任务的调度
// static Scheduler* t_scheduler = nullptr  表示这个变量在整个进程中是共享的
Scheduler* Scheduler::GetThis()
{
	return t_scheduler;
}

void Scheduler::SetThis()
{
	t_scheduler = this;
}

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name):
m_useCaller(use_caller), m_name(name)
{
	assert(threads>0 && Scheduler::GetThis()==nullptr);

	SetThis();   // 当前调度器实例设置为全局调度器

	Thread::SetName(m_name);

	// 使用主线程当作工作线程
	if(use_caller)
	{
		threads --;  //工作线程数量

		// 创建主协程，当前协程为主协程
		Fiber::GetThis();

		// 创建调度协程
		m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false)); // false -> 该调度协程退出后将返回主协程
		Fiber::SetSchedulerFiber(m_schedulerFiber.get());    //将创建的调度协程设置为调度器的调度协程
		
		m_rootThread = Thread::GetThreadId();
		m_threadIds.push_back(m_rootThread);
	}

	m_threadCount = threads;
	if(debug) std::cout << "Scheduler::Scheduler() success\n";
}

Scheduler::~Scheduler()
{
	assert(stopping()==true);
	if (GetThis() == this) 
	{
        t_scheduler = nullptr;
    }
    if(debug) std::cout << "Scheduler::~Scheduler() success\n";
}

void Scheduler::start()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if(m_stopping)
	{
		std::cerr << "Scheduler is stopped" << std::endl;
		return;
	}

	assert(m_threads.empty());
	m_threads.resize(m_threadCount);
	for(size_t i=0;i<m_threadCount;i++)
	{
		m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
		m_threadIds.push_back(m_threads[i]->getId());
	}
	if(debug) std::cout << "Scheduler::start() success\n";
}

void Scheduler::run()  //理解见我的记录文档
{
	int thread_id = Thread::GetThreadId();
	if(debug) std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;
	
	set_hook_enable(true);

	SetThis();    // 设置当前线程的调度器实例

	// 运行在新创建的线程 -> 需要创建主协程
	if(thread_id != m_rootThread)
	{
		// 创建子线程的主协程
		Fiber::GetThis();
	}

	std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
	ScheduleTask task;
	
	while(true)
	{
		task.reset();
		bool tickle_me = false;   //通知其他线程

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			auto it = m_tasks.begin();
			// 1 遍历任务队列
			while(it!=m_tasks.end())
			{
				if(it->thread!=-1&&it->thread!=thread_id)  // 任务不属于当前线程
				{
					it++;
					tickle_me = true;    // 指定了调度线程，但不是在当前线程上调度，标记⼀下需要通知其他线程进⾏调度
					continue;
				}

				// 2 取出任务
				assert(it->fiber||it->cb);
				task = *it;
				m_tasks.erase(it); 
				m_activeThreadCount++;
				break;
			}
			tickle_me = tickle_me || (it != m_tasks.end());
		}

		if(tickle_me)
		{
			tickle();
		}

		// 3 执行任务
		if(task.fiber)    //指向一个已经存在的协程对象
		{
			{					
				std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
				if(task.fiber->getState()!=Fiber::TERM)
				{
					task.fiber->resume();	
				}
			}
			m_activeThreadCount--;
			task.reset();
		}
		else if(task.cb)  //回调函数，需要创建一个新的协程对象
		{
			std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);  // 创建一个新的协程，执行任务
			{
				std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
				cb_fiber->resume();			
			}
			m_activeThreadCount--;
			task.reset();	
		}
		else // 4 任务队列为空 -> 执行空闲协程
		{		
			// 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
            if (idle_fiber->getState() == Fiber::TERM) 
            {
            	if(debug) std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
                break;
            }
			m_idleThreadCount++;
			idle_fiber->resume();				
			m_idleThreadCount--;
		}
	}
	
}

void Scheduler::stop()
{
	if(debug) std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;
	
	if(stopping()) // 调度器已经在停止过程中
	{
		return;
	}

	m_stopping = true;	

	//如果调度器使用了主线程（m_useCaller 为 true），则确保当前调度器是自身（GetThis() == this）。
	//如果调度器没有使用主线程，则确保当前调度器不是自身（GetThis() != this）。
    if (m_useCaller) 
    {
        assert(GetThis() == this);
    } 
    else 
    {
        assert(GetThis() != this);
    }
	
	for (size_t i = 0; i < m_threadCount; i++) 
	{
		tickle();  // 调用 tickle() 函数，唤醒所有工作线程，使它们能够检查停止标志并退出
	}

	if (m_schedulerFiber)   // 如果存在调度协程（m_schedulerFiber），则唤醒它，使其能够检查停止标志并退出。
	{
		tickle();
	}

	if(m_schedulerFiber)
	{
		m_schedulerFiber->resume();  // 使其恢复执行并检查停止标志，最终退出。
		if(debug) std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
	}

	std::vector<std::shared_ptr<Thread>> thrs;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		thrs.swap(m_threads);
	}

	for(auto &i : thrs)
	{
		i->join(); 
	}
	if(debug) std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;
}

void Scheduler::tickle()
{
}

void Scheduler::idle()
{
	while(!stopping())
	{
		if(debug) std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;	
		sleep(1);	
		Fiber::GetThis()->yield(); //让出CPU资源，等待下一次调度
	}
}

bool Scheduler::stopping() //调度器是否应该停止
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}

}