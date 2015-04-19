#ifndef HTTPCONN_H
#define HTTPCONN_H
#include <arpa/inet.h>
#include <sys/stat.h>

/**
**HTTP服务类
*/
class HttpConn
{
    public:
		//文件名的最大长度
        static const int FILENAME_LEN=200;
		//读缓冲区的大小
        static const int READ_BUFFER_SIZE=2048;
		//写缓冲区的大小
        static const int WRITE_BUFFER_SIZE=1024;
		//HTTP请求方法
        enum METHOD{GET=0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT,PATCH};
		//解析客户请求时，主状态机的状态
        enum CHECK_STATE{CHECK_STATE_REQUESTLINE=0,
                                                    CHECK_STATE_HEADER,
                                                    CHECK_STATE_CONTENT};
		//处理HTTP请求的结果
        enum HTTP_CODE{NO_REQUEST,GET_REQUEST,BAD_REQUEST,NO_RESOURCE,FORBIDDEN_REQUEST,FILE_REQUEST,INTERNAL_ERROR,CLOSED_CONNECTION};
        //行的读取状态
		enum LINE_STATUS{LINE_OK=0,LINE_BAD,LINE_OPEN};
    public:
		//epoll文件描述符，所有事件使用相同的
        static int m_epollfd_;
		//用户数量
        static int m_user_count_;
    public:
		//初始化
        HttpConn();
		//销毁
        virtual ~HttpConn();
    public:
		//初始化新接受的连接
        void Init(int sockfd,const struct sockaddr_in &addr);
		//关闭连接
	    void Close(bool real_close=true);
		//处理客户请求	
	    void Process();
		//非阻塞读操作
        bool Read();
		//非阻塞写操作
        bool Write();
    protected:
    private:
		//HTTP连接的socket
        int m_sockfd_;
		//客户端的ip地址
        struct sockaddr_in m_address_;
		//读缓冲区
        char m_read_buf[READ_BUFFER_SIZE];
		//标识读缓冲区已经读入的客户端数据的最后一个字节的下一个位置
	    int m_read_idx_;
		//当前正在分析的字符在读缓冲区中的位置
        int m_checked_idx_;
		//当前正在解析的行的起始位置
        int m_start_line_;
		//写缓冲区
        char m_write_buf[WRITE_BUFFER_SIZE];
		//写缓冲区中待发送的字节数
		int m_write_idx_;
		//主状态机当前状态
        CHECK_STATE m_check_state_;
		//请求方法
        METHOD m_method_;
        //客户请求文件的完整路径
		char m_real_file[FILENAME_LEN];
        //客户请求文件的文件名
		char* m_url_;
		//HTTP版本协议号
        char* m_version_;
		//主机名
        char* m_host_;
		//HTTP请求的消息体长度
        int m_content_length_;
		//HTTP请求是否要保持连接
        bool m_linger_;
		//客户请求的目标文件被mmap到内存的起始位置
        char* m_file_address_;
		//目标文件的状态
        struct stat m_file_stat_;
		//成员iov_base指向一个缓冲区，存放readv所接收的数据或是writev将要发送的数据
		//iov_len确定接收的最大长度以及实际写入的长度
        struct iovec m_iv[2];
		//内存块的数量
        int m_iv_count_;
    private:
		//初始化连接
        void Init();
		//解析HTTP请求
        HTTP_CODE ProcessRead();
		//填充HTTP应答
        bool ProcessWrite(HTTP_CODE ret);
		//解析HTTP请求行，获得请求方法，目标URL，以及HTTP版本号
        HTTP_CODE ParseRequestLine(char *text);
		//解析HTTP请求的一个头部信息
        HTTP_CODE ParseHeaders(char *text);
		//解析HTTP请求的消息体
        HTTP_CODE ParseContent(char *text);
		//分析目标文件属性
        HTTP_CODE DoRequest();
		//获取内容
        char *GetLine()
        {
            return m_read_buf+m_start_line_;
        }
		//从状态机
        LINE_STATUS ParseLine();
		//解除内存映射
        void Unmap();
		//向写缓冲写入待发送的数据
        bool AddResponse(const char *format,...);
		//发送的内容
        bool AddContent(const char *content);
		//发送的状态
        bool AddStatusLine(int status,const char *title);
		//发送的头部
        bool AddHeaders(int content_length);
		//消息体长度
        bool AddContentLength(int content_length);
		//状态信息
        bool AddLinger();
		//标志信息
        bool AddBlankLine();
};
#endif // HTTPCONN_H
