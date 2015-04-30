#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include "HttpConn.h"

const char* ok_200_title="OK";
const char* error_400_title="Bad Request";
const char* error_400_form="You request had bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title="Forbidden";
const char* error_403_form="You do not have permission to get file from this server.\n";
const char* error_404_title="Not Found";
const char* error_404_form="The requested file was not found on this server.\n";
const char* error_500_title="Internal Error";
const char* error_500_form="There was an unusual problem serving the requested file.\n";

const char* doc_root="/var/www/html";

/*设置文件描述符为非阻塞*/
int SetNonblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

/*将文件描述符添加到epoll内核事件表中，one_shot表示是否只触发一次该事件，防止多个线程处理同一个事件*/
void AddFd(int epollfd,int fd,bool one_shot)
{
    struct epoll_event event;
    event.data.fd=fd;
    /*可读，设置事件为ET模式，ET （edge-triggered）是高速工作方式，只支持no-block socket，
    **连接断开，或处于半关闭状态
    */
    event.events=EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    SetNonblocking(fd);
}

/*移除事件表中的文件描述符*/
void RemoveFd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
}

/*修改事件表中的文件描述符*/
void ModFd(int epollfd,int fd,int ev)
{
    struct epoll_event event;
    event.data.fd=fd;
    event.events=ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int HttpConn::m_user_count_=0;
int HttpConn::m_epollfd_ =-1;

HttpConn::HttpConn()
{
}

HttpConn::~HttpConn()
{
}

void HttpConn::Close(bool real_close)
{
    if(real_close && (m_sockfd_ !=-1))
    {
        RemoveFd(m_epollfd_,m_sockfd_);
        m_sockfd_=-1;
        m_user_count_--;
    }
}

void HttpConn::Init(int sockfd,const struct sockaddr_in& addr)
{
    m_sockfd_=sockfd;
    m_address_=addr;
   /*注释部分避免超时*/
//    int reuse=1;
//    setsockopt(m_sockfd_,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    AddFd(m_epollfd_,sockfd,true);
    m_user_count_++;
    Init();
}

void HttpConn::Init()
{
    /*主状态机当前状态*/
    m_check_state_=CHECK_STATE_REQUESTLINE;
    /*Http请求是否 保持连接*/
    m_linger_=false;
    /*方法*/
    m_method_=GET;
    /*客户请求文件的文件名*/
    m_url_=0;
    /*HTTP版本协议号*/
    m_version_=0;
    /*HTTP请求的消息体长度*/
    m_content_length_=0;
    /*主机名*/
    m_host_=0;
    /*当前正在解析的行的起始位置*/
    m_start_line_=0;
    /*当前正在分析的字符在读缓冲区中的位置*/
    m_checked_idx_=0;
    /*标识读缓冲区已经读入的客户端数据的最后一个字节的下一个位置*/
    m_read_idx_=0;
    /*写缓冲区中待发送的字节数*/
    m_write_idx_=0;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}

char *HttpConn::GetLine()
{
    return m_read_buf+m_start_line_;
}

HttpConn::LINE_STATUS HttpConn::ParseLine()
{
    char temp;
    /*m_checked_idx_指向buffer中正在分析的字节，m_read_idx_指向buffer中客户数据的尾部的下一字节*/
    for(;m_checked_idx_<m_read_idx_;++m_checked_idx_)
    {
        /*获得当前需要分析的字节*/
        temp=m_read_buf[m_checked_idx_];
        /*如果当前字节是\r，则可能是一个完整的行*/
        if(temp=='\r')
        {
            /*\r是最后一个数据，需要进一步分析*/
            if((m_checked_idx_+1)==m_read_idx_)
            {
                return LINE_OPEN;
            }
            /*读取到\n，是一个完整的行*/
            else if(m_read_buf[m_checked_idx_+1]=='\n')
            {
                m_read_buf[m_checked_idx_++]='\0';
                m_read_buf[m_checked_idx_++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        /*当前是\n，分析前一个是否是\r，判断是否是完整的行*/
        else if(temp=='\n')
        {
            if((m_checked_idx_>1)&&(m_read_buf[m_checked_idx_-1]=='\r'))
            {
                m_read_buf[m_checked_idx_-1]='\0';
                m_read_buf[m_checked_idx_++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

bool HttpConn::Read()
{
    if(m_read_idx_>=READ_BUFFER_SIZE)
        return false;
    int bytes_read=0;
    while(true)
    {
        bytes_read=recv(m_sockfd_,m_read_buf+m_read_idx_,READ_BUFFER_SIZE-m_read_idx_,0);
        if(bytes_read==-1)
        {
            if(errno==EAGAIN || errno ==EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if(bytes_read==0)
        {
            return false;
        }
        m_read_idx_+=bytes_read;
    }
    return true;
}
//解析HTTP请求行，获得请求方法，目标URL，以及HTTP版本号
HttpConn::HTTP_CODE HttpConn::ParseRequestLine(char* text)
{
    /*比较字符串str1和str2中是否有相同的字符，如果有，则返回该字符在str1中的位置的指针*/
    m_url_=strpbrk(text," \t");
    /*请求行中无空格或者\t，则请求有问题*/
    if(!m_url_)
    {
        return BAD_REQUEST;
    }
    *m_url_++='\0';
    char* method=text;
    if(strcasecmp(method,"GET")==0)
    {
        m_method_=GET;
    }
    else
    {
        return BAD_REQUEST;
    }
    /*返回字符串中第一个不在指定字符串中出现的字符下标*/
    m_url_+=strspn(m_url_," \t");
    m_version_=strpbrk(m_url_," \t");
    if(!m_version_)
    {
        return BAD_REQUEST;
    }
    *m_version_++='\0';
    /*忽略大小写比较字符串*/
    if(strcasecmp(m_version_,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url_,"http://",7)==0)
    {
        m_url_+=7;
        /*查找字符串s中首次出现字符c的位置*/
        m_url_=strchr(m_url_,'/');
    }

    if(!m_url_ || m_url_[0]!='/')
    {
        return BAD_REQUEST;
    }
    /*HTTP请求行处理完毕，状态转移到头部字段的分析*/
    m_check_state_=CHECK_STATE_HEADER;
    return NO_REQUEST;
}


/*解析头部信息*/
HttpConn::HTTP_CODE HttpConn::ParseHeaders(char* text)
{
    /*遇到一个空行，得到一个正确的HTTP请求*/
    if(text[0] == '\0')
    {
        if(m_content_length_!=0)
        {
            m_check_state_=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Connection:",11)==0)
    {
        text+=11;
        text+=strspn(text,"  \t");
        if(strcasecmp(text,"keep-alive")==0)
        {
            m_linger_=true;
        }
    }
    else if(strncasecmp(text,"Content-Length",15)==0)
    {
        text+=15;
        text+=strspn(text," \t");
        /*把字符串转换成长整型数*/
        m_content_length_=atol(text);
    }
    else if(strncasecmp(text,"Host:",5)==0)
    {
        text+=5;
        text+=strspn(text," \t");
        m_host_=text;
    }
    else
    {
        printf("Unknow header %s.\n",text);
    }
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::ParseContent(char* text)
{
    if(m_read_idx_>=(m_content_length_+m_checked_idx_))
    {
        text[m_content_length_]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::ProcessRead()
{
    /*记录当前行的读取状态*/
    LINE_STATUS line_status=LINE_OK;
    /*记录HTTP请求的处理结果*/
    HTTP_CODE ret=NO_REQUEST;
    char *text=0;
    while(((m_check_state_==CHECK_STATE_CONTENT)&&(line_status==LINE_OK)) || ((line_status=ParseLine())==LINE_OK))
    {
        text=GetLine();
        /*记录下一行的起始位置*/
        m_start_line_=m_checked_idx_;
        printf("Got 1 http line: %s.\n",text);
        switch(m_check_state_)
        {
            /*分析请求行*/
            case CHECK_STATE_REQUESTLINE:
            {
                ret=ParseRequestLine(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            /*分析头部字段*/
            case CHECK_STATE_HEADER:
            {
                ret=ParseHeaders(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if(ret==GET_REQUEST)
                {
                    return DoRequest();
                }
                line_status=LINE_OPEN;
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret=ParseContent(text);
                if(ret==GET_REQUEST)
                {
                    return DoRequest();
                }
                line_status=LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::DoRequest()
{
    /*分析请求文件的完整路径及文件是否存在*/
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);
    strncpy(m_real_file+len,m_url_,FILENAME_LEN-len-1);
    if(stat(m_real_file,&m_file_stat_)<0)
    {
        return NO_RESOURCE;
    }
    if(!(m_file_stat_.st_mode&S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat_.st_mode))
    {
        return BAD_REQUEST;
    }
    int fd=open(m_real_file,O_RDONLY);
    /*将文件地址映射到m_file_address_中*/
    m_file_address_=(char*)mmap(0,m_file_stat_.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;
}

void HttpConn::Unmap()
{
    if(m_file_address_)
    {
        munmap(m_file_address_,m_file_stat_.st_size);
        m_file_address_=0;
    }
}

bool HttpConn::Write()
{
    int temp;
    int bytes_have_send=0;
    int bytes_to_send=m_write_idx_;
    if(bytes_to_send==0)
    {
        ModFd(m_epollfd_,m_sockfd_,EPOLLIN);
        Init();
        return true;
    }
    while(true)
    {
        temp=writev(m_sockfd_,m_iv,m_iv_count_);
        if(temp<=-1)
        {
            /*如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，服务器无法立即接受同一客户的下一个请求*/
            if(errno==EAGAIN)
            {
                ModFd(m_epollfd_,m_sockfd_,EPOLLOUT);
                return true;
            }
            Unmap();
            return false;
        }
        bytes_to_send-=temp;
        bytes_have_send+=temp;
        if(bytes_to_send<=bytes_have_send)
        {
            Unmap();
            if(m_linger_)
            {
                Init();
                ModFd(m_epollfd_,m_sockfd_,EPOLLIN);
                return true;
            }
            else
            {
                ModFd(m_epollfd_,m_sockfd_,EPOLLIN);
                return false;
            }
        }
    }
}

bool HttpConn::AddResponse(const char* format,...)
{
    if(m_write_idx_>=WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list,format);
    int len=vsnprintf(m_write_buf+m_write_idx_,WRITE_BUFFER_SIZE-1-m_write_idx_,format,arg_list);
    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx_))
    {
        return false;
    }
    m_write_idx_+=len;
    va_end(arg_list);
    return true;
}

bool HttpConn::AddStatusLine(int status,const char* title)
{
    return AddResponse("%s %d %s\r\n","HTTP/1.1",status,title);
}

bool HttpConn::AddHeaders(int content_len)
{
    return AddContentLength(content_len) && AddLinger() && AddBlankLine();
}

bool HttpConn::AddContentLength(int content_len)
{
    return AddResponse("Content-Length: %d\r\n",content_len);
}

bool HttpConn::AddLinger()
{
    return AddResponse("Connection: %s \r\n",(m_linger_==true)?"keep-alive":"close");
}

bool HttpConn::AddBlankLine()
{
    return AddResponse("%s","\r\n");
}

bool HttpConn::AddContent(const char* content)
{
    return AddResponse("%s",content);
}

bool HttpConn::ProcessWrite(HTTP_CODE ret)
{
    switch(ret)
    {
        case INTERNAL_ERROR:
        {
            AddStatusLine(500,error_500_title);
            AddHeaders(strlen(error_500_form));
            if(!AddContent(error_500_form))
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            AddStatusLine(400,error_400_title);
            AddHeaders(strlen(error_400_title));
            if(!AddContent(error_400_form))
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            AddStatusLine(404,error_404_title);
            AddHeaders(strlen(error_404_title));
            if(!AddContent(error_404_form))
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            AddStatusLine(403,error_403_title);
            AddHeaders(strlen(error_403_title));
            if(!AddContent(error_403_form))
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            AddStatusLine(200,ok_200_title);
            if(m_file_stat_.st_size!=0)
            {
                AddHeaders(m_file_stat_.st_size);
                m_iv[0].iov_base=m_write_buf;
                m_iv[0].iov_len=m_write_idx_;
                m_iv[1].iov_base=m_file_address_;
                m_iv[1].iov_len=m_file_stat_.st_size;
                m_iv_count_=2;
                return true;
            }
            else
            {
                const char * ok_string="<html><body></body></html>";
                AddHeaders(strlen(ok_string));
                if(!AddContent(ok_string))
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }
    m_iv[0].iov_base=m_write_buf;
    m_iv[0].iov_len=m_write_idx_;
    m_iv_count_=1;
    return true;
}

void HttpConn::Process()
{
    HTTP_CODE read_ret=ProcessRead();
    if(read_ret==NO_REQUEST)
    {
        ModFd(m_epollfd_,m_sockfd_,EPOLLIN);
        return;
    }
    bool write_ret=ProcessWrite(read_ret);
    if(!write_ret)
    {
        Close();
    }
    ModFd(m_epollfd_,m_sockfd_,EPOLLOUT);
}
