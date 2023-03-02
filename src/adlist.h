/* adlist.h - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

typedef struct listNode {
    struct listNode *prev;     //ldc:前置节点
    struct listNode *next;     //ldc:后置节点
    void *value;     //ldc:节点的值
} listNode;

typedef struct listIter {
    listNode *next;
    int direction;
} listIter;

typedef struct list {
    listNode *head;     //ldc:链表头部
    listNode *tail;     //ldc:链表尾部
    void *(*dup)(void *ptr);        //ldc:节点值复制
    void (*free)(void *ptr);        //ldc:节点值释放
    int (*match)(void *ptr, void *key);     //ldc:节点值比较
    unsigned long len;      //ldc:节点个数
} list;

/* Functions implemented as macros */
#define listLength(l) ((l)->len)        //ldc:链表长度
#define listFirst(l) ((l)->head)        //ldc:链表头部
#define listLast(l) ((l)->tail)        //ldc:链表尾部
#define listPrevNode(n) ((n)->prev)        //ldc:前置节点
#define listNextNode(n) ((n)->next)        //ldc:后置节点
#define listNodeValue(n) ((n)->value)        //ldc:节点的值

#define listSetDupMethod(l,m) ((l)->dup = (m))        //ldc:设置节点复制函数指针
#define listSetFreeMethod(l,m) ((l)->free = (m))        //ldc:设置节点释放函数指针
#define listSetMatchMethod(l,m) ((l)->match = (m))        //ldc:设置节点比较函数指针
#define listGetDupMethod(l) ((l)->dup)
#define listGetFreeMethod(l) ((l)->free)
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes */
list *listCreate(void);        //ldc:创建链表
void listRelease(list *list);        //ldc:释放链表
void listEmpty(list *list);        //ldc:清空链表
list *listAddNodeHead(list *list, void *value);        //ldc:头部添加节点
list *listAddNodeTail(list *list, void *value);        //ldc:尾部添加节点
list *listInsertNode(list *list, listNode *old_node, void *value, int after);        //ldc:在after后面插入节点
void listDelNode(list *list, listNode *node);        //ldc:删除添加节点
listIter *listGetIterator(list *list, int direction);        //ldc:list迭代器
listNode *listNext(listIter *iter);        //ldc:迭代节点
void listReleaseIterator(listIter *iter);        //ldc:释放迭代器
list *listDup(list *orig);        //ldc:复制list
listNode *listSearchKey(list *list, void *key);        //ldc:按值查找节点
listNode *listIndex(list *list, long index);        //ldc:获取第index个节点
void listRewind(list *list, listIter *li);        //ldc:重置为正向迭代器
void listRewindTail(list *list, listIter *li);        //ldc:重置为逆向迭代器
void listRotateTailToHead(list *list);      //ldc:将尾结点放置到首部
void listRotateHeadToTail(list *list);      //ldc:将第一个结点放置到最后
void listJoin(list *l, list *o);        //ldc:将链表o接到l的后方

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __ADLIST_H__ */
