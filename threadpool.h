/*
*        程序名：threadpool.h
*	     功能：定义基于可变参模板实现的线程池类所用的属性和方法声明
*	     作者：zy
*/
#ifndef THREADPOOL_H
#define THREADPPPL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <condition_variable>
#include <unordered_map>

// Any类型：可以接收任意数据类型
class Any
{
public:
    // 默认构造
    Any() = default;
    // 默认析构
    ~Any() = default;

    // 禁止左值引用拷贝
    Any(const Any&) = delete;
    // 禁止左值引用赋值
    Any& operator=(const Any&) = delete;

    //允许右值引用的拷贝
    Any(Any&&) = default;
    //允许右值引用的赋值
    Any& operator=(Any&&) = default;

    // 接受任意数据的构造
    template<typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data)) {}

    // 把Any对象里存储的data数据提取出来
    template<typename T>
    T cast_()
    {
        // 如何从base_里面找到它指向的派生类对象 从它里面取出data变量?
        // 基类指针强制转成派生类指针 RTTI类型识别
        Derive<T> *pd = dynamic_cast<Derive<T>*>(base_.get());
        if (pd == nullptr)
        {
            throw "type is unmatch!";
        }
        return pd->data_;
    }
private:
    // 基类类型
    class Base
    {
    public:
        virtual ~Base() = default; // 需要使用虚函数
    };
    // 派生类类型
    template<typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data) {}
        T data_; // 保存了其他类型
    };
private:
    // 定义一个基类的指针，可以指向派生类对象
    std::unique_ptr<Base> base_;
};

// 实现Semaphore类
class Semaphore
{
public:
    // 构造
    Semaphore(int resLimit = 0) : resLimit_(resLimit) {}

    // 析构
    ~Semaphore() = default;

    // 获取一个信号量资源
    void acquire_()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        //等待信号量有资源，没有资源就阻塞当前线程
        cond_.wait(lock, [&]()->bool { return resLimit_>0;});
        resLimit_--;
    }

    // 释放一个信号量资源
    void release_()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        cond_.notify_all();
    }
private:
    int resLimit_; // 资源计数
    std::mutex mtx_;
    std::condition_variable cond_;
};

//Task类型的前置声明
class Task;
// 接收提交到线程池task任务执行完成后的返回值类型Result
class Result
{
public:
    // 构造
    Result(std::shared_ptr<Task> task, bool isValid = true);
    // 析构
    ~Result() = default;
    // 获取任务执行完的返回值记录在any_中，并通过信号量通知其他线程任务执行完成
    void setVal(Any any);
    // 用户调用该方法获取task的返回值
    Any get();

private:
    Any any_; // 存储任务的返回值
    Semaphore sem_; // 线程通信信号量
    std::shared_ptr<Task> task_; //指向获取任务返回值的任务对象
    std::atomic_bool isValid_; // 返回值是否有效
};

// 任务抽象基类
class Task
{
public:
    Task();
    ~Task() = default;
    // 用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
    virtual Any run() = 0;
    // 执行任务并把任务的返回值通过srtVal()给到Result
    void exec();
    // 设置Result
    void setResult(Result* res);
private:
    // 不能用智能指针，会造成强智能指针的交叉引用问题，导致指针无法释放造成内存泄漏
    // Result对象的生命周期大于Task的，因此不用担心指针没了
    Result* result_; 
};

// 线程池支持类型
enum class PoolMode
{
    MODE_FIXED,  // 固定数量的线程
    MODE_CACHED, // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型,参数类型为int,返回值类型为void
    using ThreadFunc = std::function<void(int)>;

    // 线程构造
    Thread(ThreadFunc func);

    // 线程析构
    ~Thread();

    // 启动线程
    void start();

    // 获取线程ID
	int getId() const;

private:
    ThreadFunc func_;
    static int generatedId_;
    int threadId_;
};

/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
public:
    void run() { // 线程代码... }
};

pool.submitTask(std::make_shared<MyTask>());
*/
// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool();

    // 线程池析构
    ~ThreadPool();

    // 设置线程池工作模式
    void setMode(PoolMode mode);

    // 设置任务队列最大阈值
    void setTaskQueMaxThreshold(int threshold);

    // 设置线程池cached模式下线程阈值
    void setThreadSizeThreshold(int threshold);

    // 给线程池提交任务
    Result submitTask(std::shared_ptr<Task> task);

    // 开启线程池，初始化最大线程数量为CPU核心个数
    void start(int initThreadSize = std::thread::hardware_concurrency());

    // 禁止拷贝构造
    ThreadPool(const ThreadPool&) = delete;

    // 禁止赋值
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // 定义线程函数,线程池决定线程执行什么函数，将threadFunc函数用绑定器绑定成函数对象
    void threadFunc(int threadId);
    // 检查线程池运行状态
    bool checkRunningState() const;

private:
    std::unordered_map<int,std::unique_ptr<Thread>> threads_; // 有映射关系的线程列表
    size_t initThreadSize_; // 初始线程数量
    std::atomic_int idleThreadSize_; // 记录空闲线程数
    std::atomic_int curThreadSize_; // 记录当前线程池里面的线程总数量
    size_t threadSizeThreshold_; // 线程列表中的最大线程数

    std::queue<std::shared_ptr<Task>> taskQue_; // 任务队列，有的任务可能是临时的，将已经析构的任务存入队列中毫无意义,因此用智能指针,拉长对象生命周期并可以自动释放资源
    std::atomic_uint taskSize_; // 任务数量，用原子操作保证任务队列线程安全（多个线程都要用到因此用原子类型）
    int taskQueMaxThreshold_; // 任务队列最大任务数量
    
    std::mutex taskQueMtx_; // 保证任务队列线程安全
    std::condition_variable notFull_; // 表示任务队列不满
    std::condition_variable notEmpty_; // 表示任务队列不空 
    std::condition_variable exitCond_; // 等待线程资源全部回收

    PoolMode poolMode_; // 当前线程池工作模式
    std::atomic_bool isPoolRunning_; //表示当前线程池的启动状态（多个线程都要用到因此用原子类型）
};

#endif