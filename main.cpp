#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include "ThreadPool.h"
#include "HttpConn.h"

//最大文件描述符
#define MAX_FD 65536
//最大事件数
#define MAX_EVENT_NUMBER 10000

//定义添加需要监听的文件描述符，是否设置为只能被一个线程操作
extern int AddFd(int epollfd,int fd,bool one_shot);
//移除监听的文件描述符
extern int RemoveFd(int epollfd,int fd);

//设置信号的处理函数
void AddSig(int sig,void(handler)(int),bool restart=true)
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    if(restart)
    {
        sa.sa_flags|=SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL)!=-1);
}

//输出错误信息
void ShowError(int connfd,const char* info)
{
    printf("%s",info);
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int main()
{
    const char* ip="0.0.0.0";
    int port=8080;
	//忽略SIGPIPE信号
    AddSig(SIGPIPE,SIG_IGN);
    ThreadPool<HttpConn>* pool=NULL;
    try
    {
        pool=new ThreadPool<HttpConn>;
    }
    catch(...)
    {
        return 1;
    }
	//预先为可能的客户分配连接对象
    HttpConn* users=new HttpConn[MAX_FD];
    assert(users);
    //int user_count=0;
    int listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);
	//设置连接的断开方式，强制退出
    struct linger tmp={1,0};
	//设置套接口选项
    setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    int ret=0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family=AF_INET;
    inet_pton(AF_INET,ip,&address.sin_addr);
    address.sin_port=htons(port);
    ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    assert(ret>=0);
    ret=listen(listenfd,5);
    assert(ret>=0);
    struct epoll_event events[MAX_EVENT_NUMBER];
    int epollfd=epoll_create(5);
    assert(epollfd!=-1);
    AddFd(epollfd,listenfd,false);
    HttpConn::m_epollfd_=epollfd;
    while(true)
    {
        int number=epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if((number<0)&&(errno!=EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        for(int i=0;i<number;++i)
        {
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength=sizeof(client_address);
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
                if(connfd<0)
                {
                    printf("errno is:%d\n",errno);
                    continue;
                }
                if(HttpConn::m_user_count_>=MAX_FD)
                {
                    ShowError(connfd,"Internal server busy");
                    continue;
                }
				//初始化客户连接
                users[connfd].Init(connfd,client_address);
            }
			//异常，直接关闭连接
            else if(events[i].events &(EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                users[sockfd].Close();
            }
            else if(events[i].events & EPOLLIN)
            {
                if(users[sockfd].Read())
                {
                    pool->Append(users+sockfd);
                }
                else
                {
                    users[sockfd].Close();
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                if(!users[sockfd].Write())
                {
                    users[sockfd].Close();
                }
            }
            else
            {
            }
        }
    }
    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;
    return 0;
}
