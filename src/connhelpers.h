
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

#ifndef __REDIS_CONNHELPERS_H
#define __REDIS_CONNHELPERS_H

#include "connection.h"

/* These are helper functions that are common to different connection
 * implementations (currently sockets in connection.c and TLS in tls.c).
 *
 * Currently helpers implement the mechanisms for invoking connection
 * handlers and tracking connection references, to allow safe destruction
 * of connections from within a handler.
 */

/* Increment connection references.
 *
 * Inside a connection handler, we guarantee refs >= 1 so it is always
 * safe to connClose().
 *
 * In other cases where we don't want to prematurely lose the connection,
 * it can go beyond 1 as well; currently it is only done by connAccept().
 */
static inline void connIncrRefs(connection *conn) {
    conn->refs++;
}

/* Decrement connection references.
 *
 * Note that this is not intended to provide any automatic free logic!
 * callHandler() takes care of that for the common flows, and anywhere an
 * explicit connIncrRefs() is used, the caller is expected to take care of
 * that.
 */

static inline void connDecrRefs(connection *conn) {
    conn->refs--;
}

static inline int connHasRefs(connection *conn) {
    return conn->refs;
}

/* Helper for connection implementations to call handlers:
 * 1. Increment refs to protect the connection.
 * 2. Execute the handler (if set).
 * 3. Decrement refs and perform deferred close, if refs==0.
 */
static inline int callHandler(connection *conn, ConnectionCallbackFunc handler) {       //ldc:只是一个辅助函数，用于调用回调函数 handler.   conn的生命周期，因为这里是依靠引用计数维持conn的生命周期，因此每次在将conn作为参数时，都需要调用一次 connIncrRefs(conn);来增加引用，防止在 回调函数 handler中 conn被释放。比如 handler中又包含了一个 callHandler，那么没有这个增加引用计数，则潜藏着在第二个 callHandler中 conn就会关闭，导致返回到第一个 callHandler中时conn就失效了
    connIncrRefs(conn);
    if (handler) handler(conn);     //ldc:连接时调用回调函数clientAcceptHandler,普通操作时调用readQueryFromClient
    connDecrRefs(conn);
    if (conn->flags & CONN_FLAG_CLOSE_SCHEDULED) {
        if (!connHasRefs(conn)) connClose(conn);        //ldc:如果客户端conn没有引用了,则直接关闭客户端.  注意:整个代码中，只有两处有判断条件 if (!connHasRefs(conn)) ：函数 connClose 和 callHandler，最后也肯定是在 callHandler 下面if 中关闭的
        return 0;       //ldc:至此，服务端处理完了客户端连接请求，主要过程如下:1、先是为这个客户cfd创建一个连接对象conn，保存了客户端文件描述符conn->fd以及当前连接状态conn->state 2、为这个连接创建一个客户端对象c，还要为这个连接注册可读事件，设置读取回调事件为readQueryFromClient。毕竟客户端需要处理很多事情，并且将这个客户端对象保存在conn->private_data。此时，conn的状态是 CONN_STATE_ACCEPTING 3、对这个客户端其他部分进行初始化 4、上面都完成了，那么就是创建完成，状态就变成CONN_STATE_ACCEPTING转为CONN_STATE_CONNECTED
    }
    return 1;
}

#endif  /* __REDIS_CONNHELPERS_H */
