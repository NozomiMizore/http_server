#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include "locker.h"
class http_conn
{
public:
    static const int FILENAME_LEN = 200;        //文件名最大长度
    static const int READ_BUFFER_SIZE = 2048;   //读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;  //写缓冲区大小
    /*
    本项目实际使用的只有GET
    HTTP/1.1支持以下9种method
    GET: 请求指定的页面信息，并返回实体主体。
    HEAD: 类似于 GET 请求，只不过返回的响应中没有具体的内容，仅获取报头
    POST: 向指定资源提交数据进行处理请求（例如提交表单或者上传文件）。数据被包含在请求体中。
            POST 请求可能会导致新的资源的建立和/或已有资源的修改
    PUT: 从客户端向服务器传送的数据取代指定的文档的内容
    DELETE: 请求服务器删除指定的页面。
    CONNECT: HTTP/1.1协议中预留给能够将连接改为管道方式的代理服务器。
    OPTIONS: 允许客户端查看服务器的性能。
    TRACE: 回显服务器收到的请求，主要用于测试或诊断。
    PATCH: 是对 PUT 方法的补充，用来对已知资源进行局部更新 。
    */
    enum METHOD {GET = 0, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH};
    /*
    主状态机的状态(当前行的类型)：
    CHECK_STATE_REQUESTLINE: 正在解析请求行
    CHECK_STATE_HEADER: 正在解析请求头部
    CHECK_STATE_CONTENT: 正在解析消息体
    */
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};
    /*
    报文解析的结果可能类型如下：
    NO_REQUEST: 请求不完整，需要继续读取请求报文数据
    GET_REQUEST: 获得了完整的HTTP请求
    BAD_REQUEST: 请求报文有语法错误或请求资源为目录
    NO_RESOURCE: 请求的资源不存在
    FORBIDDEN_REQUEST：没有权限访问请求的资源
    FILE_REQUEST: 请求的资源是文件且可正常访问
    INTERNAL_ERRORl: 服务器内部错误
    CLOSED_CONNECTION: 申请的http连接已关闭
    */
    enum HTTP_CODE {NO_REQUEST = 0, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};
    /* 
    从状态机（当前行的读取状态）可能有以下三种状态:
    LINE_OK: 完整读取了一行
    LINE_BAD: 行语法/格式有误
    LINE_OPEN: 读取的行不完整
    */
	enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};

    http_conn(){}
    ~http_conn(){}

    void init(int sockfd, const sockaddr_in& addr); //初始化套接字地址，函数内部会调用私有方法init
    void close_conn(); //关闭http连接
    void process(); //主从状态机 报文解析（处理客户端请求）
    bool read(); //读取浏览器端发来的全部数据 非阻塞读
    bool write(); //响应报文写入函数 非阻塞写

private:
    void init(); // 初始化连接
    HTTP_CODE process_read(); //从read_buf读取，并处理请求报文
    bool process_write(HTTP_CODE ret); //向write_buf写入响应报文数据

    HTTP_CODE parse_request_line(char* text); //主状态机解析报文中的请求行数据
    HTTP_CODE parse_headers(char* text); //主状态机解析报文中的请求头数据
    HTTP_CODE parse_content(char* text); //主状态机解析报文中的请求体数据
    HTTP_CODE do_request(); //生成响应报文
    char* get_line() {return m_read_buf + m_start_line;} //get_line用于将指针向后偏移，指向未处理的字符
    LINE_STATUS parse_line(); //读取一行，分析是请求报文的哪一部分

    void unmap();  //封装munmap
    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_content_type();
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE]; // 读缓冲区
    int m_read_idx; //标识读缓冲区中已经读入数据的字节数
    int m_checked_idx; //当前正在分析的字符在读缓冲区中的位置
    int m_start_line; //当前正在解析的行的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE]; //写缓冲区
    int m_write_idx; // 写缓冲区中待发送的字节数

	
    CHECK_STATE m_check_state; //主状态机的状态
    METHOD m_method; //请求方法

    //以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN]; // 客户请求的目标文件的完整路径
    char* m_url;
    char* m_version;
    char* m_host;
    int m_content_length;
    bool m_linger; //是否保持连接

	
    char* m_file_address;       //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;    //对应文件的filestat
    struct iovec m_iv[2];       //io向量机制iovec
    int m_iv_count;             // m_iv_count表示被写内存块的数量
    
    int bytes_to_send;                  // 将要发送的数据的字节数
    int bytes_have_send;                // 已经发送的字节数
    
};

#endif
