#ifndef LOCKER_H
#define LOCKER_H
#include <semaphore.h>
/**
**信号量操作类
*/
class Sem
{
    public:
	//创建并初始化信号量
        Sem();
	//销毁信号量
        virtual ~Sem();
	//等待信号量
        bool Wait();
	//增加信号量
        bool Post();
    protected:
    private:
        sem_t m_sem_;
};

/**
**互斥锁操作类
*/
class Locker
{
    public:
	//创建并初始化互斥锁
        Locker();
	//销毁互斥锁
        virtual ~Locker();
	//加锁
        bool Lock();
	//解锁
        bool Unlock();
    protected:
    private:
        pthread_mutex_t m_mutex_;
};

/**
**条件变量操作类
*/
class Cond
{
public:
	//创建并初始化条件变量
	Cond();
	//销毁条件变量
	virtual ~Cond();
	//等待条件变量
	bool Wait();
	//唤醒等待条件变量的线程
	bool Signal();
private:
	pthread_mutex_t m_mutex_;
	pthread_cond_t m_cond_;
};
#endif // LOCKER_H
