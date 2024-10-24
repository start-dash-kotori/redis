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

// 相当于是一个HashMap 的 Node，保存了 k-v 和 next，附加了其他元数据
typedef struct dictEntry {
    void *key;
    // union 的作用是可以在同一个内存地址上存储多个相同内存大小的数据类型，但是只能存储一个
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next;     /* Next entry in the same hash bucket. */
    void *metadata[];           /* An arbitrary number of bytes (starting at a
                                 * pointer-aligned address) of size as returned
                                 * by dictType's dictEntryMetadataBytes(). */
} dictEntry;

typedef struct dict dict;

/**
* dictType 定义了一组函数指针，这些函数指针用于处理字典中的键和值的各种操作。<br/>
* 通过这种方式，可以为不同的数据类型和需求定制不同的字典行为。<br/>
* 类比 Java 相当于提出了一个 interface，这个 interface 定义了一系列操作，
* 在不同的 struct 中声明这个参数相当于实现了这个 interface，
* 在实例化的时候可以动态指定这几个方法应该用什么方式来执行，这比 interface 更灵活。
*/
typedef struct dictType {
    // 计算 hash
    uint64_t (*hashFunction)(const void *key);
    // 复制 key
    void *(*keyDup)(dict *d, const void *key);
    // 复制 value
    void *(*valDup)(dict *d, const void *obj);
    // 比较 key
    int (*keyCompare)(dict *d, const void *key1, const void *key2);
    // 释放 key
    void (*keyDestructor)(dict *d, void *key);
    // 释放 value
    void (*valDestructor)(dict *d, void *obj);
    // 是否允许 dict 扩容
    int (*expandAllowed)(size_t moreMem, double usedRatio);
    /* Allow a dictEntry to carry extra caller-defined metadata.  The
     * extra memory is initialized to 0 when a dictEntry is allocated. */
    // 每个字典条目附加的元数据字节数
    size_t (*dictEntryMetadataBytes)(dict *d);
} dictType;

// 通过 exp 计算出 hash table 的大小
#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1<<(exp))
// 计算出 hash table 的大小掩码，用于计算 hash table 的 idx
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp))-1)

struct dict {
    // 指定了 dict 的一系列操作，相当于实现了 dictType 的接口
    dictType *type;

    // 两个 dictEntry 头数组，参考 HashMap 中的 Node 数组
    // 每个 dictEntry 都有一个 next 指针，指向下一个 dictEntry
    // 数组组织了 hash 的 idx，头下面的 next 指向了这个 idx 下的所有节点
    // idx 和下面的子节点合起来是一个 bucket
    dictEntry **ht_table[2];
    // 当前两个表已经使用的数量
    unsigned long ht_used[2];
    // 两个 hash 表大小的指数，获取实际大小通过宏定义来计算
    signed char ht_size_exp[2]; /* exponent of size. (size = 1<<exp) */

    // 渐进式 rehash 的进度，idx=-1 时就是没有在 rehash
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */

    /* Keep small vars at end for optimal (minimal) struct padding */
    // 若 >0 则表示 rehash暂停， <0 是编码错误
    int16_t pauserehash; /* If >0 rehashing is paused (<0 indicates coding error) */
};

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;
    long index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    unsigned long long fingerprint;
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(dict *d, dictEntry **bucketref);

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_EXP      2
#define DICT_HT_INITIAL_SIZE     (1<<(DICT_HT_INITIAL_EXP))

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d), (entry)->v.val)

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

#define dictMetadata(entry) (&(entry)->metadata)
#define dictMetadataSize(d) ((d)->type->dictEntryMetadataBytes \
                             ? (d)->type->dictEntryMetadataBytes(d) : 0)

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) (DICTHT_SIZE((d)->ht_size_exp[0])+DICTHT_SIZE((d)->ht_size_exp[1]))
#define dictSize(d) ((d)->ht_used[0]+(d)->ht_used[1])
#define dictIsRehashing(d) ((d)->rehashidx != -1)
#define dictPauseRehashing(d) (d)->pauserehash++
#define dictResumeRehashing(d) (d)->pauserehash--

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long) genrand64_int64())
#else
#define randomULong() random()
#endif

typedef enum {
    DICT_RESIZE_ENABLE,
    DICT_RESIZE_AVOID,
    DICT_RESIZE_FORBID,
} dictResizeEnable;

/* API */
//// 创建和初始化
dict *dictCreate(dictType *type);

//// 扩容/缩容
// 会强行把容量调整到指定大小
int dictExpand(dict *d, unsigned long size);
// 尝试扩展字典的容量，如果当前容量足够则不扩展
int dictTryExpand(dict *d, unsigned long size);
// 调整字典的大小，通常在删除大量元素后调用
int dictResize(dict *d);
// 设置是否允许扩容
void dictSetResizeEnabled(dictResizeEnable enable);

//// 插入/更新
int dictAdd(dict *d, void *key, void *val);
// 仅添加 key
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
// 向字典中添加一个键值对，如果键已存在则返回现有条目
dictEntry *dictAddOrFind(dict *d, void *key);
// 替换字典中的键值对，如果键不存在则添加
int dictReplace(dict *d, void *key, void *val);

//// 删除/释放
// 删除一个键值对
int dictDelete(dict *d, const void *key);
// 解除链接一个键值对，但不释放内存
// 为什么会有 unlink：dictUnlink 允许你在删除键值对时暂时不释放其内存，这在某些场景下非常有用，例如：
// 批量操作: 如果你需要批量删除多个键值对，可以先解除链接，然后统一释放内存，减少频繁的内存分配和释放开销。
// 性能优化: 在高并发或多线程环境下，延迟释放可以减少锁的竞争，提高性能
dictEntry *dictUnlink(dict *d, const void *key);
// 释放上面没有释放内存的键值对
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
// 释放 dict 占用的内存，相当于整个 dict 都被删除了
void dictRelease(dict *d);
// 清空字典，仅删除键值对
void dictEmpty(dict *d, void(callback)(dict*));

//// 查找
// 找到 key 对应的完整的 entry
dictEntry * dictFind(dict *d, const void *key);
// 找到 key 对应的 value
// void* 是任何类型的指针，因此返回的是任意值
void *dictFetchValue(dict *d, const void *key);

//// 遍历/迭代
dictIterator *dictGetIterator(dict *d);
// 获取安全的字典迭代器，可以在遍历过程中修改字典
dictIterator *dictGetSafeIterator(dict *d);
// 获取迭代器的下一个条目
dictEntry *dictNext(dictIterator *iter);
//释放迭代器对象
void dictReleaseIterator(dictIterator *iter);
// 扫描字典，调用回调函数对字典中的每个条目进行操作
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);

//// 随机/统计
// 获取字典中的一个随机键值对
dictEntry *dictGetRandomKey(dict *d);
// 获取字典中的一个随机键值对，但保证键值对不会重复
dictEntry *dictGetFairRandomKey(dict *d);
// 获取字典中的随机键值对，返回键值对数量
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
// 获取字典的统计信息
void dictGetStats(char *buf, size_t bufsize, dict *d);

//// Hash
// 生成键的哈希值
uint64_t dictGenHashFunction(const void *key, size_t len);
// 生成键的哈希值，不区分大小写
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len);
// 逐步重哈希字典
int dictRehash(dict *d, int n);
// 逐步重哈希字典，每次最多执行指定毫秒
int dictRehashMilliseconds(dict *d, int ms);
// 设置哈希函数的种子
void dictSetHashFunctionSeed(uint8_t *seed);
// 获取哈希函数的种子
uint8_t *dictGetHashFunctionSeed(void);
// 获取键的哈希值
uint64_t dictGetHash(dict *d, const void *key);
// 获取键的哈希值，不区分大小写
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int flags);
#endif

#endif /* __DICT_H */
