#include <iostream>
#include <chrono>
#include <fstream>
#include <string>
#include "threadpool.h"

/*
example 1:
class MyTask : public Task {
public:
    void run() {
        std::cout << "tid : " << std::this_thread::get_id() << " has begin." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cout << "tid : " << std::this_thread::get_id() << " has end." << std::endl;
    }
};
*/

/*
example 2:
有些场景是希望能够取得线程执行任务的返回值的 例如：计算1到3w的和
线程1:负责计算1到1w的和
线程2:负责计算1w到2w的和
线程3:负责计算2w到3w的和
主线程main 负责给每个线程分配计算的区间 并等待他们算完返回结果进行合并

问题描述：
1.每个任务执行的结果返回值类型不同，虚函数和模板不能同时使用，如何去设计run()的返回值去接收任意类型？
解决方案：构建一个可用接收任意类型的Any类(C++17里有Any类型)
基类指向派生类，在派生类中使用模板接收用户需要的数据，作为派生类的成员变量
Any => Base* -> Drive:public Base Drive里面有一个data 模版类型

2.如何设计Result机制？提交任务后，应该返回一个Result对象 然后用这个对象调对应的方法去获取任务的返回值 
(1)如果调用方法去获取返回值的时候，这个线程将任务执行完了，则直接获取返回值
(2)如果调用方法的时候这个线程没有执行完呢？这个方法应该阻塞住，等待线程将任务执行完毕
*/

using uLong = unsigned long long;
class MyTask : public Task
{
public:
    MyTask(uLong begin, uLong end):begin_(begin), end_(end){}
    Any run()
    {
        std::cout << "tid : " << std::this_thread::get_id() << " has begin." << std::endl;
        uLong sum = 0;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        for (uLong i = begin_; i <= end_; i++) {
            sum += i;
        }
        std::cout << "tid : " << std::this_thread::get_id() << " has end." << std::endl;
        return sum;
    }

private:
    int begin_;
    int end_;
};

int main()
{
    // 设计：当线程池出作用域析构时，此时任务队列里如果还有任务
    // 是等任务全部执行完成再结束？
    // 还是不执行剩下的任务了？（原本是不执行剩下的任务了）
    {
        ThreadPool pool;
        pool.setMode(PoolMode::MODE_CACHED); // 用户自己设置线程池工作模式    
        pool.start(2);

        Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        Result res2 = pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));
        pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
        pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
        pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));

        uLong sum1 = res1.get().cast_<uLong>();
        std::cout << sum1 << std::endl;
    }
    std::cout << "main over!" << std::endl;

    /*// example 3: 死锁分析
    {
        ThreadPool pool;
        pool.setMode(PoolMode::MODE_CACHED); // 用户自己设置线程池工作模式    
        pool.start(2);

        Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        Result res2 = pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));
        pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
        pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
        pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));

        uLong sum1 = res1.get().cast_<uLong>();
        std::cout << sum1 << std::endl;
    }
    std::cout << "main over!" << std::endl;*/

    /*// 问题：ThreadPool对象析构以后 怎么把线程池相关的线程资源全部回收？
    // example 3: 用户自定义的可增长的线程池工作模式
    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHED); // 用户自己设置线程池工作模式    
    pool.start(4);
    Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
    Result res2 = pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));
    Result res3 = pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
    pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
    pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
    pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
    uLong sum1 = res1.get().cast_<uLong>();
    uLong sum2 = res2.get().cast_<uLong>();
    uLong sum3 = res3.get().cast_<uLong>();
    std::cout << (sum1 + sum2 + sum3) << std::endl;*/

    /*//example 2: 获取用户任务的返回值
    ThreadPool pool;
    pool.start();
    // 提交任务成功时初始化一个Result(task)对象，当线程执行完后会给该对象赋值结果
    Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 10000));
    Result res2 = pool.submitTask(std::make_shared<MyTask>(10001, 20000));
    Result res3 = pool.submitTask(std::make_shared<MyTask>(20001, 30000));

    // 阻塞直到获得线程得到结果，并返回
    uLong sum1 = res1.get().cast_<uLong>(); //返回一个Any类型，怎么转成具体的类型呢？强制类型转换
    uLong sum2 = res2.get().cast_<uLong>();
    uLong sum3 = res3.get().cast_<uLong>();

    // Master - Slave线程模型
    // Master线程用来分解任务，然后给各个salve线程分配任务
    // 等待各个Slave线程执行完任务，返回结果
    // Master线程合并各个任务结果并输出
    std::cout << (sum1 + sum2 + sum3) << std::endl;*/

    getchar();
}