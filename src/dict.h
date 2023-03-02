/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
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

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0
#define DICT_ERR 1

typedef struct dictEntry {
    void *key;      //ldc:键
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;      //ldc:值
    struct dictEntry *next;     /* Next entry in the same hash bucket. */
    void *metadata[];           /* An arbitrary number of bytes (starting at a
                                 * pointer-aligned address) of size as returned
                                 * by dictType's dictEntryMetadataBytes(). */
} dictEntry;

typedef struct dict dict;

typedef struct dictType {
    uint64_t (*hashFunction)(const void *key);      //ldc:哈希函数
    void *(*keyDup)(dict *d, const void *key);      //ldc:复制键的函数
    void *(*valDup)(dict *d, const void *obj);      //ldc:复制值的函数
    int (*keyCompare)(dict *d, const void *key1, const void *key2);     //ldc:键的比较
    void (*keyDestructor)(dict *d, void *key);      //ldc:键的销毁
    void (*valDestructor)(dict *d, void *obj);      //ldc:值的销毁
    int (*expandAllowed)(size_t moreMem, double usedRatio);     //ldc:字典里的哈希表是否允许扩容
    /* Allow a dictEntry to carry extra caller-defined metadata.  The
     * extra memory is initialized to 0 when a dictEntry is allocated. */
    size_t (*dictEntryMetadataBytes)(dict *d);      //ldc:允许调用者向条目 (dictEntry) 中添加额外的元信息.这段额外信息的内存会在条目分配时被零初始化
} dictType;

#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1<<(exp))        //ldc:结果=2^exp
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp))-1)      //ldc:获取哈希表的掩码

struct dict {
    dictType *type;     //ldc:定义了元素操作的回调函数

    dictEntry **ht_table[2];        //ldc:两个全局哈希表指针数组，与渐进式 rehash 有关
    unsigned long ht_used[2];       //ldc:别表示哈希表数组中各自已经存放键值对的个数

    long rehashidx; /* rehashing not in progress if rehashidx == -1 */      //ldc:记录渐进式 rehash 进度的标志， -1 表示当前没有执行 rehash

    /* Keep small vars at end for optimal (minimal) struct padding */
    int16_t pauserehash; /* If >0 rehashing is paused (<0 indicates coding error) */        //ldc:表示 rehash 的状态，大于0时表示 rehash 暂停了，小于0表示出错了
    signed char ht_size_exp[2]; /* exponent of size. (size = 1<<exp) */     //ldc:表示两个哈希表数组的大小，通过 1 << ht_size_exp[0/1] 来计算
};

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;        //ldc:指向当前迭代的字典
    long index;     //ldc:表示指向的键值对索引
    int table, safe;        //ldc:table:是哈希表的号码，ht[0]/ht[1]  safe:表示该迭代器是否安全.安全时可以掉用 dictAdd,dictFind等等其他函数，不安全时只能调用 dictNext
    dictEntry *entry, *nextEntry;       //ldc:entry:指向迭代器所指的键值对  nextEntry:指向下一个键值对
    /* unsafe iterator fingerprint for misuse detection. */
    unsigned long long fingerprint;     //ldc:指纹, 用于检查不安全迭代器的误用
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(dict *d, dictEntry **bucketref);

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_EXP      2      //ldc:哈希表的初始大小为 2<<2=4
#define DICT_HT_INITIAL_SIZE     (1<<(DICT_HT_INITIAL_EXP))     //ldc:哈希表的初始大小为 2<<2=4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d), (entry)->v.val)     //ldc:释放 val

#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d), _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d), (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d), _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d), key1, key2) : \
        (key1) == (key2))

#define dictMetadata(entry) (&(entry)->metadata)        //ldc:获取用户提供的元数据
#define dictMetadataSize(d) ((d)->type->dictEntryMetadataBytes \
                             ? (d)->type->dictEntryMetadataBytes(d) : 0)

#define dictHashKey(d, key) (d)->type->hashFunction(key)        //ldc:获取 key 的哈希值
#define dictGetKey(he) ((he)->key)      //ldc:获取dictEntry的key
#define dictGetVal(he) ((he)->v.val)      //ldc:获取dictEntry的value
#define dictGetSignedIntegerVal(he) ((he)->v.s64)      //ldc:获取dictEntry的有符号的整型 val 值
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) (DICTHT_SIZE((d)->ht_size_exp[0])+DICTHT_SIZE((d)->ht_size_exp[1]))        //ldc:获取dict总的槽数
#define dictSize(d) ((d)->ht_used[0]+(d)->ht_used[1])       //ldc:获取dcit已经存放元素的个数
#define dictIsRehashing(d) ((d)->rehashidx != -1)       //ldc:是否正在Rehash
#define dictPauseRehashing(d) (d)->pauserehash++       //ldc:停止Rehash
#define dictResumeRehashing(d) (d)->pauserehash--       //ldc:恢复Rehash

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long) genrand64_int64())       //ldc:生产64位的随机数
#else
#define randomULong() random()
#endif

typedef enum {
    DICT_RESIZE_ENABLE,
    DICT_RESIZE_AVOID,
    DICT_RESIZE_FORBID,
} dictResizeEnable;

/* API */
dict *dictCreate(dictType *type);       //ldc:创建dict
int dictExpand(dict *d, unsigned long size);        //ldc:扩展或者创建一个新的dict
int dictTryExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);     //ldc:添加一个键值对
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);        //ldc:尝试将键插入到字典中,如果键已经在字典存在，那么返回 NULL,如果键不存在，那么程序创建新的哈希节点,将节点和键关联，并插入到字典，然后返回节点本身
dictEntry *dictAddOrFind(dict *d, void *key);       //ldc:返回一个 指定key 的 dictEntry。如果 key 已经存在，则将其 dictEntry 返回；如果不存在，则将其添加并返回
int dictReplace(dict *d, void *key, void *val);     //ldc:如果 key 不存在，则添加 key，并设置 value，函数返回1。如果 key 已经存在，则更新 value 值，函数返回0
int dictDelete(dict *d, const void *key);       //ldc:删除元素
dictEntry *dictUnlink(dict *d, const void *key);        //ldc:从dict中删除 key，但 key，value 和 dictEntry 并没有被释放，需要调用 dictFreeUnlinkedEntry 函数来释放这些资源。如果 key 在哈希表中找到了，则返回对应的 dictEntry，如果没找到则返回 NULL
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);     //ldc:释放dictEntry
void dictRelease(dict *d);      //ldc:销毁dict
dictEntry * dictFind(dict *d, const void *key);     //ldc:查找 key，找到了返回其 dictEntry；否则返回 NULL
void *dictFetchValue(dict *d, const void *key);     //ldc:根据key获取value
int dictResize(dict *d);        //ldc:缩容dict
dictIterator *dictGetIterator(dict *d);     //ldc:创建并返回给定字典的不安全迭代器
dictIterator *dictGetSafeIterator(dict *d);     //ldc:创建并返回给定节点的安全迭代器
dictEntry *dictNext(dictIterator *iter);        //ldc:根据迭代器获取下个dictEntry
void dictReleaseIterator(dictIterator *iter);       //ldc:释放迭代器
dictEntry *dictGetRandomKey(dict *d);       //ldc:随机获取其中一个dictEntry
dictEntry *dictGetFairRandomKey(dict *d);       //ldc:随机获取一个dictEntry,比dictGetRandomKey的分布好
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);  //ldc:随机获取count个dictEntry
void dictGetStats(char *buf, size_t bufsize, dict *d);
uint64_t dictGenHashFunction(const void *key, size_t len);      //ldc:默认的hash函数,对字符串进行hash,不区分大小写
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len);     //ldc:对字符串进行hash,区分大小写
void dictEmpty(dict *d, void(callback)(dict*));     //ldc:清空dict
void dictSetResizeEnabled(dictResizeEnable enable);     //ldc:设置dict可以缩容
int dictRehash(dict *d, int n);     //ldc:步渐进式 rehash
int dictRehashMilliseconds(dict *d, int ms);        //ldc:Rehash ms 毫秒
void dictSetHashFunctionSeed(uint8_t *seed);        //ldc:设置hash函数的种子
uint8_t *dictGetHashFunctionSeed(void);     //ldc:获取hash函数的种子
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);       //ldc:用于迭代dict中的元素.这个迭代器是完全无状态的.可能会返回重复的元素，不过这个问题可以很容易在应用层解决
uint64_t dictGetHash(dict *d, const void *key);     //ldc:调用dictHashKey获取hash值
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);       //ldc:使用指针+hash值去查找元素

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int flags);
#endif

#endif /* __DICT_H */
