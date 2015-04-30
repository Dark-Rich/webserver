#ifndef THREADPOOL_H
#define THREADPOOL_H
#include <pthread.h>
#include <list>
#include <stdio.h>
#include "Locker.h"
/**
**线程池模板类
*/
template<typename T>
class ThreadPool
{
    public:
        //创建并初始化线程池类，线程数，最大请求数
        ThreadPool(int thread_number=4,int max_requests=10000);
        //销毁线程池类
        virtual ~ThreadPool();
        //向请求队列添加任务
        bool Append(T* request);
        //处理函数
        static void* Worker(void *arg);
        //线程池运行
        void Run();
    protected:
    private:
        //线程数
        int m_thread_number_;
        //请求队列中允许的最大请求数
        int m_max_requests_;
        //描述线程池的数组
        pthread_t* m_threads_;
        //请求队列
        std::list<T*>m_workqueue_;
        //保护请求队列的互斥锁
        Locker m_queuelocker_;
        //信号量，是否有任务处理
        Sem m_queuestat;
        //是否结束线程
        bool m_stop_;
};

template<typename T>
ThreadPool<T>::ThreadPool(int thread_number,int max_requests):m_thread_number_(thread_number),m_max_requests_(max_requests),m_stop_(false),m_threads_(NULL)
{
    if((thread_number<=0) || (max_requests<=0))
    {
        throw std::exception();
    }
    m_threads_=new  pthread_t[m_thread_number_];
    if(!m_threads_)
    {
        throw std::exception();
    }

    for(int i=0;i<thread_number;++i)
    {
        printf("Create the %dth thread.\n",i);
        if(pthread_create(m_threads_+i,NULL,Worker,this)!=0)
        {
            delete []m_threads_;
            throw std::exception();
        }
        if(pthread_detach(m_threads_[i]))
        {
            delete []m_threads_;
            throw std::exception();
        }
    }
}

template<typename T>
ThreadPool<T>::~ThreadPool()
{
    delete []m_threads_;
    m_stop_=true;
}

template<typename T>
bool ThreadPool<T>::Append(T* request)
{
    m_queuelocker_.Lock();
    if(m_workqueue_.size()>m_max_requests_)
    {
        m_queuelocker_.Unlock();
        return false;
    }
    m_workqueue_.push_back(request);
    m_queuelocker_.Unlock();
    m_queuestat.Post();
    return true;
}

template <typename T>
void* ThreadPool<T>::Worker(void* arg)
{
    ThreadPool* pool=(ThreadPool*)arg;
    pool->Run();
// return pool;
}

template<typename T>
void ThreadPool<T>::Run()
{
    while(!m_stop_)
    {
        m_queuestat.Wait();
        m_queuelocker_.Lock();
        if(m_workqueue_.empty())
        {
            m_queuelocker_.Unlock();
            continue;
        }
        T* request=m_workqueue_.front();
        m_workqueue_.pop_front();
        m_queuelocker_.Unlock();
        if(!request)
        {
            continue;
        }
        request->Process();
    }
}
#endif // THREADPOOL_H
