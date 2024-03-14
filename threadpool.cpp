/*
*        程序名：threadpool.cpp
*	     功能：基于模板的可变参动态线程池类的方法实现
*	     作者：zy
*/
#include "threadpool.h"

const int TASK_MAX_THRESHOLD = 1024; // 任务队列最大任务数
const int THREAD_MAX_THRESHOLD = 100; // 线程队列最大线程数
const int THREAD_MAX_IDLE_TIME = 10; // 线程最大空闲时间，单位：秒

/*************************线程池类方法实现*************************/
// 线程池构造
ThreadPool::ThreadPool()
    : initThreadSize_(4)
    , taskSize_(0)
    , taskQueMaxThreshold_(TASK_MAX_THRESHOLD)
    , idleThreadSize_(0)
    , curThreadSize_(0)
    , threadSizeThreshold_(THREAD_MAX_THRESHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
    , isPoolRunning_(false)
{}

// 线程池析构
ThreadPool::~ThreadPool()
{
    isPoolRunning_ = false;

    // 放在这里有概率出现死锁
    // 只能唤醒正在wait的线程
    // 正在执行任务中的线程没有wait就不能唤醒，但在下一次while的判断中会不符合条件退出循环，进而释放线程
    // 第三种情况：pool先获取锁，然后进入wait状态把mutex释放掉
    // 然后线程函数获取锁，并notEmpty_.wait(),没有人能唤醒它，陷入死锁
    // 第四种情况：线程池里的线程先获取锁，然后进入wait状态把mutex释放掉
    // 然后pool拿到锁，却不释放notEmpty_，陷入死锁
    // notEmpty_.notify_all(); // 唤醒处于等待状态的线程

    // 等待线程池里所有线程返回
    // 两种状态： 1、阻塞 2、执行任务中
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    notEmpty_.notify_all(); // 唤醒处于等待状态的线程
    exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
}

// 设置线程池工作模式
void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState()) return;
    poolMode_ = mode;
}

// 设置任务队列最大阈值
void ThreadPool::setTaskQueMaxThreshold(int threshold)
{
    if (checkRunningState()) return;
    taskQueMaxThreshold_ = threshold;
}

// 设置线程池cached模式下线程阈值
void ThreadPool::setThreadSizeThreshold(int threshold)
{
    if (checkRunningState()) return;
    if (poolMode_ == PoolMode::MODE_CACHED)
        threadSizeThreshold_ = threshold;
}

// 给线程池提交任务--用户调用该接口，传入任务对象，生产任务
Result ThreadPool::submitTask(std::shared_ptr<Task> task)
{
    // 获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // 线程通信：条件变量释放锁并等待任务队列有空余
    // 用户提交任务，最长不能阻塞超过1s,否则判断提交任务失败，返回
    // 使用lambda表达式判断是否要wait()
    if(!notFull_.wait_for(lock, std::chrono::seconds(1), 
        [&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshold_;}))
    {
        // 表示not_Full_等待1s，条件依然没有满足，输出日志并返回
        std::cerr << "task queue is full, submit task fail." << std::endl;
        return Result(task, false);
    }
    
    // 如果有空余，把任务放入任务队列中
    taskQue_.emplace(task);
    taskSize_++;  //submitTask()所在的线程是用户线程，而线程函数所在的线程是另外的线程，因此需要用atomic保证线程安全

    // 因为新放了任务，任务队列不空了，在not_empty_上通知赶快分配线程执行任务
    notEmpty_.notify_all();

    // MODE_CACHED模式：需要根据任务数量和空闲线程数量，判断是否需要创建新的线程
    // cached模式：场景小而快的任务；fixed模式：比较耗时的任务
    if (poolMode_ == PoolMode::MODE_CACHED
        && taskSize_ > idleThreadSize_
        && curThreadSize_ < threadSizeThreshold_)
    {
        std::cout << ">>> create new threads ..." << std::endl;
        // 创建新线程
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        // 启动线程
        threads_[threadId]->start(); 
        // 修改线程个数相关的变量
        curThreadSize_++;
        idleThreadSize_++;
    }
    // 返回任务的Result对象
    // 不推荐写成return task->getResult();
    // 因为随着task任务被执行完，task对象没了，依赖于task对象的Result对象也没了
    // Result对象的生命周期应该要设计得更长，要让用户可以调用到res.get()
    // Result(task)只要Result对象还在，task对象就还在
    return Result(task);
}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{
    // 设置线程池运行状态
    isPoolRunning_ = true;

    // 记录初始线程个数
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;

    // 创建线程对象
    for (int i = 0; i < initThreadSize_; i++)
    {
        // 创建thread线程对象的时候，把当前线程池对象的线程函数给到thread线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr)); // unique_ptr禁止左值引用的拷贝和赋值，但可以右值引用
    }
    // 启动所有线程
    for (int i = 0; i < initThreadSize_; i++)
    {
        threads_[i]->start(); // 去执行一个线程函数
        idleThreadSize_++; // 记录初始空闲线程的数量
    }
}

//定义线程函数--线程池里面的线程从任务队列中消费任务
void ThreadPool::threadFunc(int threadId)
{
    auto lastTime = std::chrono::high_resolution_clock().now();

    // 等所有任务必须执行完成，线程池才可以回收所有线程资源：for (;;)
    // 原本：while (isPoolRunning_)
    for (;;) // 死循环
    {
        std::shared_ptr<Task> task;
        {
            // 获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            std::cout << "tid : " << std::this_thread::get_id() << " 尝试获取任务..." << std::endl;

            // MODE_CACHED：有可能已经创建了很多线程，但空闲时间超过60s,应该把多余线程结束回收
            // 超过initThreadSize_数量的线程要进行回收
            // 当前时间 - 上一次线程执行的时间 > 60s

            // 每一秒返回一次 怎么区分超时返回还是有任务待执行返回 轮询
            // 原本：锁 + 双重判断 ：while (isPoolRunning_ && taskQue_.size() == 0)
            while (taskQue_.size() == 0)
            {
                // 检查是有任务被唤醒还是线程池结束回收线程资源被唤醒
                if (!isPoolRunning_)
                {
                    threads_.erase(threadId);
                    exitCond_.notify_all(); // 唤醒线程池析构函数中的条件变量
                    std::cout << "threadid : " << std::this_thread::get_id() << " exit!" << std::endl;
                    return; // 线程函数结束，线程结束
                }

                if (poolMode_ == PoolMode::MODE_CACHED)
                {
                    // 条件变量超时返回
                    if (std::cv_status::timeout == 
                    notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        if (dur.count() >= THREAD_MAX_IDLE_TIME
                            && curThreadSize_ > initThreadSize_)
                        {
                            // 回收当前线程
                            // 记录线程数量的相关变量的值修改
                            // 把线程对象从线程列表容器中删除
                            // 通过线程id找到线程对象进而删除
                            threads_.erase(threadId);
                            curThreadSize_--;
                            idleThreadSize_--;
                            std::cout << "threadid : " << std::this_thread::get_id() << " exit!" << std::endl;
                            return;
                        }
                    }
                }
                else // Fixed模式
                {
                    // 等待notEmpty_条件，任务队列size()大于 0 时不等待
                    notEmpty_.wait(lock);
                }   

                // // 检查是有任务被唤醒还是线程池结束回收线程资源被唤醒
                // if (!isPoolRunning_)
                // {
                //     threads_.erase(threadId);
                //     exitCond_.notify_all(); // 唤醒线程池析构函数中的条件变量
                //     std::cout << "threadid : " << std::this_thread::get_id() << " exit!" << std::endl;
                //     return; // 结束线程函数就是结束当前线程了
                // }             
            }

            // // 检查是有任务被唤醒还是线程池结束回收线程资源被唤醒
            // if (!isPoolRunning_)
            // {
            //     break;
            // }

            // 线程开始忙了，当前空闲线程减1
            idleThreadSize_--;

            std::cout << "tid : " << std::this_thread::get_id() << " 获取任务成功..." << std::endl;

            // 从任务队列中取一个任务出来
            task = taskQue_.front();
            taskQue_.pop();
            taskSize_--; 

            // 如果本线程取出一个任务后仍然有剩余任务，继续通知其他线程执行任务
            if (!taskQue_.empty())
            {
                notEmpty_.notify_all();
            }
            // 通知生产者可以继续提交任务
            notFull_.notify_all();           
        }// 离开作用域释放锁

        // 当前线程负责执行这个任务
        if (task != nullptr)
        {
            // 执行任务并把任务的返回值通过srtVal()给到Result
            task->exec();
            // 输出
            std::cout << "执行任务结束" << std::endl;
        }
        // 已完成任务，当前空闲线程加1
        idleThreadSize_++;
        // 更新线程执行完任务的时间
        lastTime = std::chrono::high_resolution_clock().now();
    }
    // threads_.erase(threadId);
    // std::cout << "threadid : " << std::this_thread::get_id() << " exit!" << std::endl;
    // exitCond_.notify_all(); // 唤醒线程池析构函数中的条件变量
}

// 检查线程池运行状态
bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}
/*************************线程类方法实现*************************/
int Thread::generatedId_ = 0; // 静态成员变量类外初始化

// 线程构造
Thread::Thread(ThreadFunc func)
    : func_(func)
    , threadId_(generatedId_++)
{}

// 线程析构
Thread::~Thread() {}

// 启动线程
void Thread::start()
{
    // 创建一个线程对象来执行线程函数，并向线程函数func_传递参数threadId_
    std::thread t(func_,threadId_);
    // 设置分离线程
    // 因为线程对象t离开了作用域会消失，而t又和线程函数func_绑定在了一起，所以要分离线程
    // 在调用detach()后，std::thread对象t不再与实际执行线程函数func_的线程相关联。
    t.detach();

}

// 获取线程ID
int Thread::getId() const
{
    return threadId_;
}

/*************************任务类方法实现*************************/
// 构造
Task::Task() : result_(nullptr) {}
// 执行任务并把任务的返回值通过srtVal()给到Result
void Task::exec()
{
    // 多态调用
    if (result_ != nullptr)
        result_->setVal(run());
}
// 设置Result
void Task::setResult(Result* res)
{
    result_ = res;
}
/*************************Result类方法实现*************************/
// 构造
Result::Result(std::shared_ptr<Task> task, bool isValid)
    : task_(task)
    , isValid_(isValid)
{
    task->setResult(this);
}

// 线程池线程获取任务执行完的返回值记录在any_中，并通过信号量通知用户线程任务执行完成
void Result::setVal(Any any)
{
    // 存储task的返回值
    this->any_ = std::move(any);
    // 已经获取任务的返回值，增加信号量资源
    sem_.release_();
}

// 用户调用该方法获取task的返回值
Any Result::get()
{
        if(!isValid_) return "";
        sem_.acquire_(); //任务如果没有执行完，阻塞用户线程
        return std::move(any_);
}
