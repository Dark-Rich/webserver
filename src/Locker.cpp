#include <exception>
#include <pthread.h>
#include "Locker.h"

Locker::Locker()
{
		if(pthread_mutex_init(&m_mutex_,NULL)!=0)
		{
			throw std::exception();
		}
}

Locker::~Locker()
{
    //pthread_mutex_destory(&m_mutex_);
}

bool Locker::Lock()
{
	return pthread_mutex_lock(&m_mutex_)==0;
}

bool Locker::Unlock()
{
	return pthread_mutex_unlock(&m_mutex_)==0;
}


Sem::Sem()
{
   if(sem_init(&m_sem_,0,0) != 0)
    {
        throw std::exception();
    }
}

Sem::~Sem()
{
    sem_destroy(&m_sem_);
}

bool Sem::Wait()
{
    return sem_wait(&m_sem_) == 0;
}

bool Sem::Post()
{
    return sem_post(&m_sem_) ==0;
}

Cond::Cond()
{
	if(pthread_mutex_init(&m_mutex_,NULL)!=0)
	{
		throw std::exception();
	}
	if(pthread_cond_init(&m_cond_,NULL)!=0)
	{
//		pthread_mutex_destory(&m_mutex_);
		throw std::exception();
	}
}

Cond::~Cond()
{
//    pthread_mutex_destory(&m_mutex_);
//	pthread_mutex_destory(&m_cond_);
}

bool Cond::Wait()
{
	int ret=0;
	pthread_mutex_lock(&m_mutex_);
	ret=pthread_cond_wait(&m_cond_,&m_mutex_);
	pthread_mutex_unlock(&m_mutex_);
	return ret==0;
}

bool Cond::Signal()
{
	return pthread_cond_signal(&m_cond_)==0;
}
