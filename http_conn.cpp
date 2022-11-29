#include "http_conn.h"

// 响应状态信息
const char* ok_200_title = "200 OK";
const char* error_400_title = "400 Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "403 Forbidden";
const char* error_403_form = "You do not have enough permission to get file from this server.\n";
const char* error_404_title = "404 Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "500 Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
// 网站根目录
const char* doc_root = "./www/";
// 传入fd设置为非阻塞IO
int setnonblocking(int fd){
    int old_option = fcntl( fd, F_GETFL ); //fcntl针对描述符提供控制
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}
// 将需要监听的socket加入epoll例程
void addfd(int epollfd, int fd, bool one_shot){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot){
        event.events |= EPOLLONESHOT; // 防止不同的线程或者进程在处理同一个SOCKET的事件
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd); // 设置文件描述符非阻塞
}

// 将fd从epoll例程中移除
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP; // 再把EPOLLONESHOT加回来（因为已经触发过一次了） 
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event ); // 参数准备齐全，修改指定的epoll文件描述符上的事件
}

int http_conn::m_user_count = 0; // 记录所有的客户数
int http_conn::m_epollfd = -1; // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的

//关闭http连接
void http_conn::close_conn(){
    if( m_sockfd != -1){
        //modfd( m_epollfd, m_sockfd, EPOLLIN );
        removefd(m_epollfd, m_sockfd); // 将m_sockfd从m_epollfd中移除，不再监听
        m_sockfd = -1; // 标记作用，-1代表已关闭
        m_user_count--;  // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init( int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;

    // 端口复用
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
	
    addfd( m_epollfd, sockfd, true );
    m_user_count++;

    init(); // 调用自身的重载函数，设置其他参数
}

void http_conn::init(){
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态为检查请求行
    m_linger = false; // 默认不保持连接  Connection : keep-alive保持连接

    m_method = GET; // 默认请求方式为GET
    m_url = 0;
    m_version = 0; 
    m_content_length = 0; 
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0; 
    m_read_idx = 0;
    m_write_idx = 0;
    memset( m_read_buf, '\0', READ_BUFFER_SIZE );
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE );
    memset( m_real_file, '\0', FILENAME_LEN );
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read(){
    if( m_read_idx >= READ_BUFFER_SIZE ){
        return false;
    }

    int bytes_read = 0;
    while( true ){   
        // 从m_read_buf + m_read_idx索引处开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1){
            if( errno == EAGAIN || errno == EWOULDBLOCK ){
                // 没有数据
                break;
            }
            return false;
        }
        else if ( bytes_read == 0 ){ // 对方关闭连接
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}

//从状态机，读取当前行，返回对应状态
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    // m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
    // m_checked_idx指向从状态机当前正在分析的字节
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ){
		// 获得当前要分析的字节
        temp = m_read_buf[m_checked_idx];
		// 如果当前的字节是"\r"，则说明可能读取到一个完整的行
        if ( temp == '\r' ){
            // 达到了buffer结尾，则接收不完整，需要继续接收
            if ( ( m_checked_idx + 1 ) == m_read_idx ){
                return LINE_OPEN;
            }
            // 下一个字符是\n，将\r\n改为\0\0
            else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ){
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        // 如果当前的字节是"\n"，则说明也可能读取到一个完整的行
        // 一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if( temp == '\n' ){
            //前一个字符是\r，则接收完整
            if( ( m_checked_idx > 1 ) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ){
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line( char* text )
{
    // 请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk( text, " \t" );
	// 如果请求行中没有空白字符或“\t”，则报文格式有问题
    if (! m_url){
        return BAD_REQUEST;
    }
    *m_url++ = '\0'; // 用于将前面的数据取出

    char* method = text;  // 取出数据 确定请求方式
    // 忽略大小写比较
    if ( strcasecmp( method, "GET" ) == 0 ){
        m_method = GET;
    }
    else{
        return BAD_REQUEST;
    }

    // m_url此时跳过了第一个空格或者\t字符，但是后面还可能存在
    // 不断后移找到请求资源的第一个字符
    // m_url += strspn( m_url, " \t" );
    // 判断http的版本号
    m_version = strpbrk( m_url, " \t" );
    if ( ! m_version ){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    //m_version += strspn( m_version, " \t" );
	// 仅支持HTTP/1.1
    if ( strcasecmp( m_version, "HTTP/1.1" ) != 0 ){
        return BAD_REQUEST;
    }
	// 对请求资源的前七个字符进行判断，某些带有http://的报文进行单独处理
    if ( strncasecmp( m_url, "http://", 7 ) == 0 ){
        m_url += 7;
        m_url = strchr( m_url, '/' );
    }
    // 不符合规则的报文
    if ( ! m_url || m_url[ 0 ] != '/' ){
        return BAD_REQUEST;
    }
    //当url为/时，显示首页
    if (strlen(m_url) == 1)
        strcat(m_url, "index.html");

    m_check_state = CHECK_STATE_HEADER; // 状态转移
    return NO_REQUEST;
}
// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers( char* text ){
	// 遇到空行，表示头部字段解析完毕
    if( text[ 0 ] == '\0' )
    {
        if ( m_method == HEAD )
        {
            return GET_REQUEST;
        }
        if ( m_content_length != 0 )
        {
            m_check_state = CHECK_STATE_CONTENT; // post请求需要状态转移
            return NO_REQUEST; // 请求不完整，需要继续读取客户数据
        }
		// 否则说明我们已经得到了一个完整的GET请求
        return GET_REQUEST;
    }
	/*处理头部Connection字段*/
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 )
    {
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 )
        {
            m_linger = true;
        }
    }
	/*处理Content-Length字段*/
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 )
    {
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol( text );
    }
	/*处理Host头部字段*/
    else if ( strncasecmp( text, "Host:", 5 ) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
    else
    {
        printf( "oop! unknow header %s\n", text );
    }

    return NO_REQUEST;

}
//判断http请求的消息体是否被完整读入，POST请求会调用这个函数解析消息体
http_conn::HTTP_CODE http_conn::parse_content( char* text ){
    //判断是否读取了消息体
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//主状态机，取出完整的行进行解析
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;	// 当前的读取状态
    HTTP_CODE ret = NO_REQUEST;	// HTTP请求的处理结果
    char* text = 0;
	// 从m_read_buffer中取出完整的行， 从状态机驱使主状态机
    while ( (m_check_state == CHECK_STATE_CONTENT  && line_status == LINE_OK)
               || ( ( line_status = parse_line() ) == LINE_OK ) ){
        text = get_line();

        // m_start_line是每一个数据行在m_read_buf中的起始位置
        // m_checked_idx表示从状态机在m_read_buf中的读取位置
        m_start_line = m_checked_idx;
        printf( "got a http line: %s\n", text );

        switch (m_check_state)
        {
            // 解析请求行
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            // 解析请求头
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)
                    return do_request(); // 需要跳转到报文响应函数
                break;
            }
            // 解析消息体
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                    return do_request(); // 需要跳转到报文响应函数
                line_status = LINE_OPEN; //更新 跳出循环 代表解析完了消息体
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

// 返回对请求目标文件的分析结果
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy( m_real_file, doc_root ); //将初始化的m_real_file赋值为网站根目录
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 没有想要的文件
    if ( stat( m_real_file, &m_file_stat ) < 0 )
    {
        return NO_RESOURCE;
    }
    // 没有权限读取
    if ( ! ( m_file_stat.st_mode & S_IROTH ) )
    {
        return FORBIDDEN_REQUEST;
    }
    // 请求的资源文件是目录文件
    if ( S_ISDIR( m_file_stat.st_mode ) )
    {
        return BAD_REQUEST;
    }

    int fd = open( m_real_file, O_RDONLY );
    // 通过调用mmap将文件映射到内存逻辑地址，提高访问速度
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}
//封装munmap系统调用
void http_conn::unmap(){
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write(){
    int temp = 0;
    if ( bytes_to_send == 0 ) // 将要发送的字节为0，这一次响应结束
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while( 1 )
    {
        temp = writev( m_sockfd, m_iv, m_iv_count );
        if ( temp <= -1 )
        {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN )
            {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
			/*发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接*/
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        if ( bytes_to_send <= 0 )
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            if( m_linger )
            {
                init();
                return true;
            }
            else
            {
                return false;
            } 
        }
    }
}
/*往写缓冲区写入待发送的数据*/
bool http_conn::add_response(const char* format, ...){
    if( m_write_idx >= WRITE_BUFFER_SIZE )
    {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) )
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    //printf("request:%s", m_write_buf);
    return true;
}

bool http_conn::add_status_line( int status, const char* title ){
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers( int content_len ){
    bool a = add_content_length( content_len );
    bool b = add_content_type();
    bool c = add_linger();
    bool d = add_blank_line();
    return a && b && c && d;
}

bool http_conn::add_content_length( int content_len ){
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger(){
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line(){
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content ){
    return add_response( "%s", content );
}
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}
/*根据服务器处理HTTP请求的结果，决定返回给客户端的内容*/
bool http_conn::process_write( HTTP_CODE ret ){
    switch ( ret )
    {
        case INTERNAL_ERROR:
        {
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) )
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) )
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) )
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ) );
            if ( ! add_content( error_403_form ) ){
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            // add_status_line( 200, ok_200_title );
            // if ( m_file_stat.st_size != 0 )
            // {
            //     add_headers( m_file_stat.st_size );
            //     m_iv[ 0 ].iov_base = m_write_buf;
            //     m_iv[ 0 ].iov_len = m_write_idx;
            //     m_iv[ 1 ].iov_base = m_file_address;
            //     m_iv[ 1 ].iov_len = m_file_stat.st_size;
            //     m_iv_count = 2;
            //     bytes_to_send = m_write_idx + m_file_stat.st_size;
            //     return true;
            // }
            // else
            // {
            //     const char* ok_string = "<html><body></body></html>";
            //     add_headers( strlen( ok_string ) );
            //     if ( ! add_content( ok_string ) )
            //     {
            //         return false;
            //     }
            // }
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        }
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
// 由线程中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process(){
    HTTP_CODE read_ret = process_read();
    // NO_REQUEST 表示请求不完整，需要继续接受请求数据
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN); //注册并监听读事件
        return;
    }
    //调用process_write完成报文响应
    bool write_ret = process_write( read_ret );
    if ( ! write_ret )
    {
        close_conn();
    }
    //注册并监听写事件
    modfd( m_epollfd, m_sockfd, EPOLLOUT );
}

