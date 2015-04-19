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

int setnonblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

void AddFd(int epollfd,int fd,bool one_shot)
{
    struct epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

void RemoveFd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
}

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
    //ctor
}

HttpConn::~HttpConn()
{
    //dtor
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
    int reuse=1;
    setsockopt(m_sockfd_,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    AddFd(m_epollfd_,sockfd,true);
    m_user_count_++;
    Init();
}

void HttpConn::Init()
{
    m_check_state_=CHECK_STATE_REQUESTLINE;
    m_linger_=false;
    m_method_=GET;
    m_url_=0;
    m_version_=0;
    m_content_length_=0;
    m_host_=0;
    m_start_line_=0;
    m_checked_idx_=0;
    m_read_idx_=0;
    m_write_idx_=0;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}

HttpConn::LINE_STATUS HttpConn::ParseLine()
{
    char temp;
    for(;m_checked_idx_<m_read_idx_;++m_checked_idx_)
    {
        temp=m_read_buf[m_checked_idx_];
        if(temp=='\r')
        {
            if((m_checked_idx_+1)==m_read_idx_)
            {
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_idx_+1]=='\n')
            {
                m_read_buf[m_checked_idx_++]='\0';
                m_read_buf[m_checked_idx_++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
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
    m_url_=strpbrk(text," \t");
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
    m_url_+=strspn(m_url_," \t");
    m_version_=strpbrk(m_url_," \t");
    if(!m_version_)
    {
        return BAD_REQUEST;
    }
    *m_version_++='\0';
    if(strcasecmp(m_version_,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url_,"http://",7)==0)
    {
        m_url_+=7;
        m_url_=strchr(m_url_,'/');
    }

    if(!m_url_ || m_url_[0]!='/')
    {
        return BAD_REQUEST;
    }
    m_check_state_=CHECK_STATE_HEADER;
    return NO_REQUEST;
}


//解析头部信息
HttpConn::HTTP_CODE HttpConn::ParseHeaders(char* text)
{
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
        printf("unknow header %s\n",text);
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
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char* text=0;
    while(((m_check_state_==CHECK_STATE_CONTENT)&&(line_status==LINE_OK)) || ((line_status=ParseLine())==LINE_OK))
    {
        text=GetLine();
        m_start_line_=m_checked_idx_;
        printf("got 1 http line:%s",text);
        switch(m_check_state_)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret=ParseRequestLine(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
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
    AddContentLength(content_len);
    AddLinger();
    AddBlankLine();
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
    if(!read_ret)
    {
        Close();
    }
    ModFd(m_epollfd_,m_sockfd_,EPOLLOUT);
}
