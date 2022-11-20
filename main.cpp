/*
File name:main
Author:shayne
*/
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536                //最大文件描述符数量
#define MAX_EVENT_NUMBER 10000      //最大监听事件数量

extern int addfd( int epollfd, int fd, bool one_shot );
extern int removefd( int epollfd, int fd );

// handler回调函数，用来处理信号
void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void show_error( int connfd, const char* info )
{
    printf( "%s", info );
    send( connfd, info, strlen( info ), 0 );
    close( connfd );
}


int main( int argc, char* argv[] )
{
    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );
	
	/*忽略SIGPIPE信号*/
    addsig( SIGPIPE, SIG_IGN );

    // 创建线程池
    threadpool< http_conn >* pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch( ... )
    {
        return 1;
    }

	// 预先为每个可能的用户连接分配一个http_conn对象
    http_conn* users = new http_conn[ MAX_FD ];
    assert(users);
    int user_count = 0;

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert( listenfd >= 0 );

    // struct linger tmp = { 1, 0 };
    // setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY; // INADDR_ANY转换过来是0.0.0.0→泛指本机,表示本机的所有IP(因为有些机器有多张网卡)
    address.sin_family = AF_INET;
    //inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    // 端口复用
    int reuse=1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); 

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret >= 0 );

    ret = listen( listenfd, 5 );
    assert( ret >= 0 );

    // 创建epoll对象和事件数组，并将监听的事件添加epoll中
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    // 循环处理epoll返回的事件
    while( true )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd) // 监听到了什么
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 )
                {
                    printf( "errno is: %d\n", errno );
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD ) // user_count>=65536
                {
                    show_error( connfd, "Internal server busy" );
                    continue;
                }
                /*初始化客户连接*/
                users[connfd].init( connfd, client_address );
            }
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )
            {
				/*如果有异常，直接关闭客户连接*/
                users[sockfd].close_conn();
            }
            else if( events[i].events & EPOLLIN )
            {
				/*根据读的结果，决定是将任务添加到线程池还是关闭连接*/
                if( users[sockfd].read() )
                {
                    pool->append( users + sockfd );
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if( events[i].events & EPOLLOUT ) // 对于EPOLLOUT: 如果状态改变了[比如 从满到不满],只要输出缓冲区可写就会触发
            {
				/*根据写的结果，决定是否关闭连接*/
                if( !users[sockfd].write() )
                {
                    users[sockfd].close_conn();
                }
            }
            else
            {}
        }
    }

    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}
