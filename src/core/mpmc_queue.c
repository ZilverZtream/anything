#include "core/pch.h"

#ifdef _WIN32
#include <intrin.h>
#else
#include <sched.h>
#ifndef SwitchToThread
#define SwitchToThread() sched_yield()
#endif
#ifndef _ReadWriteBarrier
#define _ReadWriteBarrier() __sync_synchronize()
#endif
#ifndef InterlockedCompareExchange64
static LONG64 InterlockedCompareExchange64(volatile LONG64* destination, LONG64 exchange, LONG64 comparand) {
    return __sync_val_compare_and_swap(destination, comparand, exchange);
}
#endif
#ifndef ZeroMemory
#define ZeroMemory(ptr, size) memset((ptr), 0, (size))
#endif
#endif

BOOL MPMC_Init(MPMCQueue* q, LONG pow2_size) {
    if(!q) return FALSE;
    LONG size = 1;
    while(size < pow2_size) size <<= 1;
    q->mask = size - 1;
    size_t cells_size = sizeof(MPMCCell) * (size_t)size;
    q->cells = (MPMCCell*)aligned_malloc(cells_size, CACHE_LINE_SIZE);
    if(!q->cells) return FALSE;
    ZeroMemory(q->cells, cells_size);
    for(LONG i = 0; i < size; ++i) {
        q->cells[i].seq = i;
    }
    q->head = 0;
    q->tail = 0;
    q->on_push = NULL;
    q->on_push_ctx = NULL;
    return TRUE;
}

void MPMC_Destroy(MPMCQueue* q) {
    if(!q || !q->cells) return;
    aligned_free(q->cells);
    q->cells = NULL;
}

BOOL MPMC_Push(MPMCQueue* q, void* data) {
    if(!q || !q->cells) return FALSE;
    MPMCCell* cell;
    LONG64 pos = q->head;
    for(;;) {
        cell = &q->cells[pos & q->mask];
        LONG64 seq = cell->seq;
        if(seq == pos) {
            if(InterlockedCompareExchange64(&q->head, pos + 1, pos) == pos) {
                cell->data = data;
                _ReadWriteBarrier();
                cell->seq = pos + 1;
                if(q->on_push) {
                    q->on_push(q->on_push_ctx);
                }
                return TRUE;
            }
        } else if(seq < pos) {
            return FALSE; // full
        } else {
            pos = q->head;
        }
        SwitchToThread();
    }
}

BOOL MPMC_Pop(MPMCQueue* q, void** out) {
    if(!q || !out || !q->cells) return FALSE;
    MPMCCell* cell;
    LONG64 pos = q->tail;
    for(;;) {
        cell = &q->cells[pos & q->mask];
        LONG64 seq = cell->seq;
        if(seq == pos + 1) {
            if(InterlockedCompareExchange64(&q->tail, pos + 1, pos) == pos) {
                void* d = cell->data;
                _ReadWriteBarrier();
                cell->seq = pos + q->mask + 1;
                *out = d;
                return TRUE;
            }
        } else if(seq < pos + 1) {
            return FALSE; // empty
        } else {
            pos = q->tail;
        }
        SwitchToThread();
    }
}

void MPMC_SetOnPush(MPMCQueue* q, void (*cb)(void*), void* ctx) {
    if(!q) return;
    q->on_push = cb;
    q->on_push_ctx = ctx;
}
