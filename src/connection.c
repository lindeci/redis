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

#include "server.h"
#include "connhelpers.h"

/* The connections module provides a lean abstraction of network connections
 * to avoid direct socket and async event management across the Redis code base.
 *
 * It does NOT provide advanced connection features commonly found in similar
 * libraries such as complete in/out buffer management, throttling, etc. These
 * functions remain in networking.c.
 *
 * The primary goal is to allow transparent handling of TCP and TLS based
 * connections. To do so, connections have the following properties:
 *
 * 1. A connection may live before its corresponding socket exists.  This
 *    allows various context and configuration setting to be handled before
 *    establishing the actual connection.
 * 2. The caller may register/unregister logical read/write handlers to be
 *    called when the connection has data to read from/can accept writes.
 *    These logical handlers may or may not correspond to actual AE events,
 *    depending on the implementation (for TCP they are; for TLS they aren't).
 */

ConnectionType CT_Socket;

/* When a connection is created we must know its type already, but the
 * underlying socket may or may not exist:
 *
 * - For accepted connections, it exists as we do not model the listen/accept
 *   part; So caller calls connCreateSocket() followed by connAccept().
 * - For outgoing connections, the socket is created by the connection module
 *   itself; So caller calls connCreateSocket() followed by connConnect(),
 *   which registers a connect callback that fires on connected/error state
 *   (and after any transport level handshake was done).
 *
 * NOTE: An earlier version relied on connections being part of other structs
 * and not independently allocated. This could lead to further optimizations
 * like using container_of(), etc.  However it was discontinued in favor of
 * this approach for these reasons:
 *
 * 1. In some cases conns are created/handled outside the context of the
 * containing struct, in which case it gets a bit awkward to copy them.
 * 2. Future implementations may wish to allocate arbitrary data for the
 * connection.
 * 3. The container_of() approach is anyway risky because connections may
 * be embedded in different structs, not just client.
 */

connection *connCreateSocket() {
    connection *conn = zcalloc(sizeof(connection));
    conn->type = &CT_Socket;
    conn->fd = -1;

    return conn;
}

/* Create a new socket-type connection that is already associated with
 * an accepted connection.
 *
 * The socket is not ready for I/O until connAccept() was called and
 * invoked the connection-level accept handler.
 *
 * Callers should use connGetState() and verify the created connection
 * is not in an error state (which is not possible for a socket connection,
 * but could but possible with other protocols).
 */
connection *connCreateAcceptedSocket(int fd) {      //ldc:创建socket-type connection,需要connAccept()才允许I/O
    connection *conn = connCreateSocket();      //ldc:申请conn空间,赋值 conn->type = &CT_Socket,  conn->fd = -1 返回conn
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING;
    return conn;
}

static int connSocketConnect(connection *conn, const char *addr, int port, const char *src_addr,
        ConnectionCallbackFunc connect_handler) {
    int fd = anetTcpNonBlockBestEffortBindConnect(NULL,addr,port,src_addr);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTING;

    conn->conn_handler = connect_handler;
    aeCreateFileEvent(server.el, conn->fd, AE_WRITABLE,
            conn->type->ae_handler, conn);

    return C_OK;
}

/* Returns true if a write handler is registered */
int connHasWriteHandler(connection *conn) {
    return conn->write_handler != NULL;
}

/* Returns true if a read handler is registered */
int connHasReadHandler(connection *conn) {
    return conn->read_handler != NULL;
}

/* Associate a private data pointer with the connection */
void connSetPrivateData(connection *conn, void *data) {
    conn->private_data = data;
}

/* Get the associated private data pointer */
void *connGetPrivateData(connection *conn) {
    return conn->private_data;
}

/* ------ Pure socket connections ------- */

/* A very incomplete list of implementation-specific calls.  Much of the above shall
 * move here as we implement additional connection types.
 */

/* Close the connection and free resources. */
static void connSocketClose(connection *conn) {     //ldc:上层有一个wrapper函数 connClose,提供给用户使用
    if (conn->fd != -1) {
        aeDeleteFileEvent(server.el,conn->fd, AE_READABLE | AE_WRITABLE);
        close(conn->fd);
        conn->fd = -1;
    }

    /* If called from within a handler, schedule the close but
     * keep the connection until the handler returns.
     */
    if (connHasRefs(conn)) {        //ldc:控制连接对象生命周期的引用计数conn->refs。如果此时引用计数不为0,说明是处于某个回调函数中,此时不能直接关闭,设置标志位 CONN_FLAG_CLOSE_SCHEDULED,需要延迟关闭
        conn->flags |= CONN_FLAG_CLOSE_SCHEDULED;
        return;
    }

    zfree(conn);
}

static int connSocketWrite(connection *conn, const void *data, size_t data_len) {       //ldc:connSocketWrite 函数上层是被connWrite函数调用。用户无法直接调用 connSocketWrite,只能通过conn对象来调用
    int ret = write(conn->fd, data, data_len);
    if (ret < 0 && errno != EAGAIN) {       //ldc:ret ==-1 && errno ==EAGAIN,在非阻塞IO是正常下的,不是错误
        conn->last_errno = errno;

        /* Don't overwrite the state of a connection that is not already
         * connected, not to mess with handler callbacks.
         */
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }

    return ret;
}

static int connSocketWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    int ret = writev(conn->fd, iov, iovcnt);
    if (ret < 0 && errno != EAGAIN) {
        conn->last_errno = errno;

        /* Don't overwrite the state of a connection that is not already
         * connected, not to mess with handler callbacks.
         */
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }

    return ret;
}

static int connSocketRead(connection *conn, void *buf, size_t buf_len) {        //ldc:connSocketRead 经过封装被 connRead调用,将数据读取到到buf中。用户不应该直接调用 connSocketRead
    int ret = read(conn->fd, buf, buf_len);
    if (!ret) {
        conn->state = CONN_STATE_CLOSED;
    } else if (ret < 0 && errno != EAGAIN) {        //ldc:ret ==-1 && errno ==EAGAIN,在非阻塞IO是正常下的,不是错误
        conn->last_errno = errno;

        /* Don't overwrite the state of a connection that is not already
         * connected, not to mess with handler callbacks.
         */
        if (errno != EINTR && conn->state == CONN_STATE_CONNECTED)
            conn->state = CONN_STATE_ERROR;
    }

    return ret;
}

static int connSocketAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    int ret = C_OK;

    if (conn->state != CONN_STATE_ACCEPTING) return C_ERR;
    conn->state = CONN_STATE_CONNECTED;     //ldc:连接状态转换为CONN_STATE_CONNECTED

    connIncrRefs(conn);
    if (!callHandler(conn, accept_handler)) ret = C_ERR;        //ldc:调用回调函数 accept_handler=clientAcceptHandler
    connDecrRefs(conn);

    return ret;
}

/* Register a write handler, to be called when the connection is writable.
 * If NULL, the existing handler is removed.
 *
 * The barrier flag indicates a write barrier is requested, resulting with
 * CONN_FLAG_WRITE_BARRIER set. This will ensure that the write handler is
 * always called before and not after the read handler in a single event
 * loop.
 */     //ldc:根据函数 connSocketSetWriteHandler 的第三个标志位barrier,可封装成两个函数给用户使用 connSetWriteHandler、connSetWriteHandlerWithBarrier
static int connSocketSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {      //ldc:主要是在epoll_wait上注册可写事件,并设置可写事件的回调函数为 ae_handler,真正执行写操作的还是func.  设置连接对象conn的写回调函数 conn->write_handler 为 func,再注册写事件,如果设置的回调函数 func为NULL,则取消注册可写事件  注意：这里注册的可写事件的回调函数是ae_handler,是因为在ae_handler中,综合处理了可读、可写事件
    if (func == conn->write_handler) return C_OK;

    conn->write_handler = func;     //ldc:设置新的可写事件处理函数
    if (barrier)
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    if (!conn->write_handler)       //ldc:如果没有设置可写事件处理函数,则取消关注可写事件
        aeDeleteFileEvent(server.el,conn->fd,AE_WRITABLE);
    else
        if (aeCreateFileEvent(server.el,conn->fd,AE_WRITABLE,       //ldc:关注可写事件
                    conn->type->ae_handler,conn) == AE_ERR) return C_ERR;
    return C_OK;
}

/* Register a read handler, to be called when the connection is readable.
 * If NULL, the existing handler is removed.
 */     //ldc:主要是注册可读事件,并设置读回调函数为 ae_handler,真正执行读取操作的还是func.     函数上层是被connSetReadHandler,用户无法直接调用
static int connSocketSetReadHandler(connection *conn, ConnectionCallbackFunc func) {        //ldc:1、conn->read_handler=func=readQueryFromClient,2、使用epoll_ctl添加监听新事件 3、对eventLoop->events[fd]的mask、rfileProc=wfileProc=conn->type->ae_handler=connSocketEventHandler、clientData=conn进行赋值
    if (func == conn->read_handler) return C_OK;

    conn->read_handler = func;
    if (!conn->read_handler)
        aeDeleteFileEvent(server.el,conn->fd,AE_READABLE);
    else
        if (aeCreateFileEvent(server.el,conn->fd,       //ldc:1、使用epoll_ctl添加监听新事件 2、对eventLoop->events[fd]的mask、rfileProc=wfileProc=conn->type->ae_handler=connSocketEventHandler、clientData=conn进行赋值
                    AE_READABLE,conn->type->ae_handler,conn) == AE_ERR) return C_ERR;
    return C_OK;
}

static const char *connSocketGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static void connSocketEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask)      //ldc:处理epllfd上可读可写事件
{
    UNUSED(el);
    UNUSED(fd);
    connection *conn = clientData;

    if (conn->state == CONN_STATE_CONNECTING &&     //ldc:针对发起连接对象
            (mask & AE_WRITABLE) && conn->conn_handler) {

        int conn_error = connGetSocketError(conn);
        if (conn_error) {
            conn->last_errno = conn_error;
            conn->state = CONN_STATE_ERROR;
        } else {
            conn->state = CONN_STATE_CONNECTED;
        }
    
        if (!conn->write_handler) aeDeleteFileEvent(server.el,conn->fd,AE_WRITABLE);        //ldc:如果没有设置可写事件的处理函数,则直接取消可写事件

        if (!callHandler(conn, conn->conn_handler)) return;     //ldc:处理新连接到来
        conn->conn_handler = NULL;
    }

    /* Normally we execute the readable event first, and the writable
     * event later. This is useful as sometimes we may be able
     * to serve the reply of a query immediately after processing the
     * query.       //ldc:通常我们先执行可读事件,然后执行可写事件,这种方式很有用,因为有时我们可以在处理查询之后立即提供查询的结果
     *
     * However if WRITE_BARRIER is set in the mask, our application is
     * asking us to do the reverse: never fire the writable event
     * after the readable. In such a case, we invert the calls.     //ldc:如果设置了 WRITE_BARRIER, 那么处理程序就反过来:在处理了可读事件之后都不触发可写事件
     * This is useful when, for instance, we want to do things
     * in the beforeSleep() hook, like fsync'ing a file to disk,
     * before replying to a client. */      //ldc:这种操作很有用,比如,当我在beforeSleep()函数中,执行一些阻塞操作,类似fsync操作
    int invert = conn->flags & CONN_FLAG_WRITE_BARRIER;

    int call_write = (mask & AE_WRITABLE) && conn->write_handler;
    int call_read = (mask & AE_READABLE) && conn->read_handler;

    /* Handle normal I/O flows */
    if (!invert && call_read) {
        if (!callHandler(conn, conn->read_handler)) return;     //ldc:相当调用callHandler(conn, readQueryFromClient)
    }
    /* Fire the writable event. */
    if (call_write) {
        if (!callHandler(conn, conn->write_handler)) return;
    }
    /* If we have to invert the call, fire the readable event now
     * after the writable one. */
    if (invert && call_read) {
        if (!callHandler(conn, conn->read_handler)) return;
    }
}

static int connSocketBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    int fd = anetTcpNonBlockConnect(NULL,addr,port);
    if (fd == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = errno;
        return C_ERR;
    }

    if ((aeWait(fd, AE_WRITABLE, timeout) & AE_WRITABLE) == 0) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = ETIMEDOUT;
    }

    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTED;
    return C_OK;
}

/* Connection-based versions of syncio.c functions.
 * NOTE: This should ideally be refactored out in favor of pure async work.
 */

static ssize_t connSocketSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncWrite(conn->fd, ptr, size, timeout);
}

static ssize_t connSocketSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncRead(conn->fd, ptr, size, timeout);
}

static ssize_t connSocketSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return syncReadLine(conn->fd, ptr, size, timeout);
}

static int connSocketGetType(connection *conn) {
    (void) conn;

    return CONN_TYPE_SOCKET;
}

ConnectionType CT_Socket = {        //ldc:这个结构是核心，后续的各种网络操作，都是通过ConnectionType中的指针调用的
    .ae_handler = connSocketEventHandler,       //ldc:主要是综合处理可读、可写事件
    .close = connSocketClose,       //ldc:关闭客户端操作
    .write = connSocketWrite,       //ldc:这是REdis中实际完成写操作的最底层的函数,调用write函数完成发送
    .writev = connSocketWritev,
    .read = connSocketRead,     //ldc:和写操作类似,这是最底层的函数,服务器读取客户端发来的数据
    .accept = connSocketAccept,
    .connect = connSocketConnect,
    .set_write_handler = connSocketSetWriteHandler,     //ldc:主要是在epoll_wait上注册可写事件,并设置可写事件的回调函数为 ae_handler,真正执行写操作的还是func.  func:1、设置连接对象conn的写回调函数 conn->write_handler 为 func,再注册写事件 2、如果设置的回调函数 func为NULL,则取消注册可写事件
    .set_read_handler = connSocketSetReadHandler,       //ldc:主要是注册可读事件,并设置读回调函数为 ae_handler,真正执行读取操作的还是func
    .get_last_error = connSocketGetLastError,
    .blocking_connect = connSocketBlockingConnect,
    .sync_write = connSocketSyncWrite,
    .sync_read = connSocketSyncRead,
    .sync_readline = connSocketSyncReadLine,
    .get_type = connSocketGetType
};


int connGetSocketError(connection *conn) {
    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);

    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    return sockerr;
}

int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port) {
    if (anetFdToString(conn ? conn->fd : -1, ip, ip_len, port, FD_TO_PEER_NAME) == -1) {
        if (conn) conn->last_errno = errno;
        return C_ERR;
    }
    return C_OK;
}

int connSockName(connection *conn, char *ip, size_t ip_len, int *port) {
    return anetFdToString(conn->fd, ip, ip_len, port, FD_TO_SOCK_NAME);
}

int connFormatFdAddr(connection *conn, char *buf, size_t buf_len, int fd_to_str_type) {
    return anetFormatFdAddr(conn ? conn->fd : -1, buf, buf_len, fd_to_str_type);
}

int connBlock(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetBlock(NULL, conn->fd);
}

int connNonBlock(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetNonBlock(NULL, conn->fd);
}

int connEnableTcpNoDelay(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetEnableTcpNoDelay(NULL, conn->fd);
}

int connDisableTcpNoDelay(connection *conn) {
    if (conn->fd == -1) return C_ERR;
    return anetDisableTcpNoDelay(NULL, conn->fd);
}

int connKeepAlive(connection *conn, int interval) {      //ldc:每个客户端设置心跳检测,通过三个选项TCP_KEEPIDLE、TCP_KEEPINTVL、TCP_KEEPCNT，改变了keeplive的默认值
    if (conn->fd == -1) return C_ERR;
    return anetKeepAlive(NULL, conn->fd, interval);      //ldc:每个客户端设置心跳检测,通过三个选项TCP_KEEPIDLE、TCP_KEEPINTVL、TCP_KEEPCNT，改变了keeplive的默认值
}

int connSendTimeout(connection *conn, long long ms) {
    return anetSendTimeout(NULL, conn->fd, ms);
}

int connRecvTimeout(connection *conn, long long ms) {
    return anetRecvTimeout(NULL, conn->fd, ms);
}

int connGetState(connection *conn) {
    return conn->state;
}

/* Return a text that describes the connection, suitable for inclusion
 * in CLIENT LIST and similar outputs.
 *
 * For sockets, we always return "fd=<fdnum>" to maintain compatibility.
 */
const char *connGetInfo(connection *conn, char *buf, size_t buf_len) {
    snprintf(buf, buf_len-1, "fd=%i", conn == NULL ? -1 : conn->fd);
    return buf;
}

