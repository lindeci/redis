
/*
 * Copyright (c) 2019, Redis Labs
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __REDIS_CONNECTION_H
#define __REDIS_CONNECTION_H

#include <errno.h>
#include <sys/uio.h>

#define CONN_INFO_LEN   32

struct aeEventLoop;
typedef struct connection connection;

typedef enum {
    CONN_STATE_NONE = 0,
    CONN_STATE_CONNECTING,        //ldc:发起 connect 连接时
    CONN_STATE_ACCEPTING,        //ldc:创建客户端时初始状态，即 accept 之前的状态
    CONN_STATE_CONNECTED,        //ldc:成功 accept 之后的状态
    CONN_STATE_CLOSED,        //ldc:关闭的状态
    CONN_STATE_ERROR        //ldc:出错了
} ConnectionState;

#define CONN_FLAG_CLOSE_SCHEDULED   (1<<0)      /* Closed scheduled by a handler */
#define CONN_FLAG_WRITE_BARRIER     (1<<1)      /* Write barrier requested */

#define CONN_TYPE_SOCKET            1
#define CONN_TYPE_TLS               2

typedef void (*ConnectionCallbackFunc)(struct connection *conn);

typedef struct ConnectionType {     //ldc:封装了客户端连接对象的一些读写、Accept和关闭连接等操作，是函数指针的结构体
    void (*ae_handler)(struct aeEventLoop *el, int fd, void *clientData, int mask);     //ldc:读写
    int (*connect)(struct connection *conn, const char *addr, int port, const char *source_addr, ConnectionCallbackFunc connect_handler);       //ldc:处理连接请求
    int (*write)(struct connection *conn, const void *data, size_t data_len);       //ldc:写
    int (*writev)(struct connection *conn, const struct iovec *iov, int iovcnt);       //ldc:写
    int (*read)(struct connection *conn, void *buf, size_t buf_len);       //ldc:读
    void (*close)(struct connection *conn);       //ldc:关闭
    int (*accept)(struct connection *conn, ConnectionCallbackFunc accept_handler);       //ldc:连接
    int (*set_write_handler)(struct connection *conn, ConnectionCallbackFunc handler, int barrier);       //ldc:设置write handler
    int (*set_read_handler)(struct connection *conn, ConnectionCallbackFunc handler);      //ldc:设置read handler
    const char *(*get_last_error)(struct connection *conn);     //ldc:获取最后一次错误
    int (*blocking_connect)(struct connection *conn, const char *addr, int port, long long timeout);        //ldc:阻塞连接
    ssize_t (*sync_write)(struct connection *conn, char *ptr, ssize_t size, long long timeout);       //ldc:异步写
    ssize_t (*sync_read)(struct connection *conn, char *ptr, ssize_t size, long long timeout);       //ldc:异步读
    ssize_t (*sync_readline)(struct connection *conn, char *ptr, ssize_t size, long long timeout);       //ldc:异步读取一行
    int (*get_type)(struct connection *conn);       //返回CONN_TYPE_SOCKET或者CONN_TYPE_TLS
} ConnectionType;

struct connection {
    ConnectionType *type;       //ldc:操作 connection 中的函数指针
    ConnectionState state;      //ldc:是一个 enum，表示连接的状态
    short int flags;        //ldc:CONN_FLAG_CLOSE_SCHEDULED 或者 CONN_FLAG_WRITE_BARRIER
    short int refs;     //ldc:引用计数，控制着这个连接对象生命周期
    int last_errno;     //ldc:最近一次的错误类型
    void *private_data;     //ldc:保存的是这个连接对应的客户端 client
    ConnectionCallbackFunc conn_handler;        //ldc:连接回调
    ConnectionCallbackFunc write_handler;       //ldc:写回调
    ConnectionCallbackFunc read_handler;        //ldc:读回调
    int fd;     //ldc:cfd
};

/* The connection module does not deal with listening and accepting sockets,
 * so we assume we have a socket when an incoming connection is created.
 *
 * The fd supplied should therefore be associated with an already accept()ed
 * socket.
 *
 * connAccept() may directly call accept_handler(), or return and call it
 * at a later time. This behavior is a bit awkward but aims to reduce the need
 * to wait for the next event loop, if no additional handshake is required.
 *
 * IMPORTANT: accept_handler may decide to close the connection, calling connClose().
 * To make this safe, the connection is only marked with CONN_FLAG_CLOSE_SCHEDULED
 * in this case, and connAccept() returns with an error.
 *
 * connAccept() callers must always check the return value and on error (C_ERR)
 * a connClose() must be called.
 */

static inline int connAccept(connection *conn, ConnectionCallbackFunc accept_handler) {     //ldc:接受连接(已经创建好的socket)
    return conn->type->accept(conn, accept_handler);        //ldc:实际上调用的回调函数是 connSocketAccept,主要完成两个任务:1、将conn的状态从CONN_STATE_ACCEPTING转变为CONN_STATE_CONNECTED 2、在callHandler中调用accept_handler函数，此处即 clientAcceptHandler，校验conn状态
}

/* Establish a connection.  The connect_handler will be called when the connection
 * is established, or if an error has occurred.
 *
 * The connection handler will be responsible to set up any read/write handlers
 * as needed.
 *
 * If C_ERR is returned, the operation failed and the connection handler shall
 * not be expected.
 */
static inline int connConnect(connection *conn, const char *addr, int port, const char *src_addr,       //ldc:连接(cluster、replication需要连接Master)
        ConnectionCallbackFunc connect_handler) {
    return conn->type->connect(conn, addr, port, src_addr, connect_handler);
}

/* Blocking connect.
 *
 * NOTE: This is implemented in order to simplify the transition to the abstract
 * connections, but should probably be refactored out of cluster.c and replication.c,
 * in favor of a pure async implementation.
 */
static inline int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {        //ldc:阻塞连接
    return conn->type->blocking_connect(conn, addr, port, timeout);
}

/* Write to connection, behaves the same as write(2).
 *
 * Like write(2), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connWrite(connection *conn, const void *data, size_t data_len) {      //ldc:写入connection
    return conn->type->write(conn, data, data_len);
}

/* Gather output data from the iovcnt buffers specified by the members of the iov
 * array: iov[0], iov[1], ..., iov[iovcnt-1] and write to connection, behaves the same as writev(3).
 *
 * Like writev(3), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connWritev(connection *conn, const struct iovec *iov, int iovcnt) {       //ldc:聚合iov[0], iov[1], ..., iov[iovcnt-1]然后写入connection
    return conn->type->writev(conn, iov, iovcnt);
}

/* Read from the connection, behaves the same as read(2).
 * 
 * Like read(2), a short read is possible.  A return value of 0 will indicate the
 * connection was closed, and -1 will indicate an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connRead(connection *conn, void *buf, size_t buf_len) {       //ldc:从connection读
    int ret = conn->type->read(conn, buf, buf_len);
    return ret;
}

/* Register a write handler, to be called when the connection is writable.
 * If NULL, the existing handler is removed.
 */
static inline int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func) {      //ldc:设置connection的write_handler为func
    return conn->type->set_write_handler(conn, func, 0);
}

/* Register a read handler, to be called when the connection is readable.
 * If NULL, the existing handler is removed.
 */
static inline int connSetReadHandler(connection *conn, ConnectionCallbackFunc func) {       //ldc:1、conn->read_handler=func=readQueryFromClient,2、使用epoll_ctl添加监听新事件 3、对eventLoop->events[fd]的mask、rfileProc=wfileProc=readQueryFromClient、clientData=conn进行赋值
    return conn->type->set_read_handler(conn, func);
}

/* Set a write handler, and possibly enable a write barrier, this flag is
 * cleared when write handler is changed or removed.
 * With barrier enabled, we never fire the event if the read handler already
 * fired in the same event loop iteration. Useful when you want to persist
 * things to disk before sending replies, and want to do that in a group fashion. */
static inline int connSetWriteHandlerWithBarrier(connection *conn, ConnectionCallbackFunc func, int barrier) {
    return conn->type->set_write_handler(conn, func, barrier);
}

static inline void connClose(connection *conn) {        //ldc:关闭connection
    conn->type->close(conn);
}

/* Returns the last error encountered by the connection, as a string.  If no error,
 * a NULL is returned.
 */
static inline const char *connGetLastError(connection *conn) {      //ldc:获取最近的error信息
    return conn->type->get_last_error(conn);
}

static inline ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {     //ldc:同步写
    return conn->type->sync_write(conn, ptr, size, timeout);
}

static inline ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {     //ldc:同步读
    return conn->type->sync_read(conn, ptr, size, timeout);
}

static inline ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {     //ldc:读一行
    return conn->type->sync_readline(conn, ptr, size, timeout);
}

/* Return CONN_TYPE_* for the specified connection */
static inline int connGetType(connection *conn) {       //返回CONN_TYPE_SOCKET或者CONN_TYPE_TLS
    return conn->type->get_type(conn);
}

static inline int connLastErrorRetryable(connection *conn) {
    return conn->last_errno == EINTR;
}

connection *connCreateSocket();     //ldc:创建Socket
connection *connCreateAcceptedSocket(int fd);       //ldc:创建、接受Socket

connection *connCreateTLS();        //ldc:创建TLS
connection *connCreateAcceptedTLS(int fd, int require_auth);        //ldc:创建、接受TLS

void connSetPrivateData(connection *conn, void *data);      //ldc:设置connection的PrivateData
void *connGetPrivateData(connection *conn);      //ldc:获取connection的PrivateData
int connGetState(connection *conn);     //ldc:获取连接状态
int connHasWriteHandler(connection *conn);      //ldc:write_handler是否有值
int connHasReadHandler(connection *conn);       //ldc:read_handler是否有值
int connGetSocketError(connection *conn);       //ldc:获取socket的error

/* anet-style wrappers to conns */      //ldc:封装anet风格到conns
int connBlock(connection *conn);      //ldc:阻塞
int connNonBlock(connection *conn);      //ldc:非阻塞
int connEnableTcpNoDelay(connection *conn);      //ldc:TCP没延迟
int connDisableTcpNoDelay(connection *conn);      //ldc:TCP可以延迟
int connKeepAlive(connection *conn, int interval);
int connSendTimeout(connection *conn, long long ms);        //ldc:发包超时设置
int connRecvTimeout(connection *conn, long long ms);        //ldc:收包超时设置
int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port);        //ldc:格式化地址
int connFormatFdAddr(connection *conn, char *buf, size_t buf_len, int fd_to_str_type);
int connSockName(connection *conn, char *ip, size_t ip_len, int *port);        //ldc:SOCKET名字
const char *connGetInfo(connection *conn, char *buf, size_t buf_len);       //ldc:获取fb

/* Helpers for tls special considerations */
sds connTLSGetPeerCert(connection *conn);
int tlsHasPendingData();
int tlsProcessPendingData();

#endif  /* __REDIS_CONNECTION_H */
