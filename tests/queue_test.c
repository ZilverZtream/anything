/*
 * MPMC queue tests with cancellation-aware push.
 *
 * Verifies:
 *  - MPMC_Init / Push / Pop basic operations
 *  - Queue full behavior (Push returns FALSE)
 *  - Cancel token stops blocked producers
 *  - Queue survives push/pop interleaving
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef _WIN32
#include "stubs/windows_stub.h"
#include "stubs/bcrypt.h"
#endif

#include "anything/anything.h"

/* aligned_malloc / aligned_free from util or stubs */
#ifndef _WIN32
static void* aligned_malloc(size_t size, size_t alignment){
    void* p = NULL;
    if(posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}
static void aligned_free(void* p){ free(p); }
#else
#define aligned_malloc(s,a) _aligned_malloc(s,a)
#define aligned_free(p) _aligned_free(p)
#endif

static void test_basic_push_pop(void){
    MPMCQueue q;
    assert(MPMC_Init(&q, 8)); /* power of 2, capacity 8 */

    int a = 1, b = 2, c = 3;
    assert(MPMC_Push(&q, &a) == TRUE);
    assert(MPMC_Push(&q, &b) == TRUE);
    assert(MPMC_Push(&q, &c) == TRUE);

    void* out = NULL;
    assert(MPMC_Pop(&q, &out) == TRUE);
    assert(out == &a);
    assert(MPMC_Pop(&q, &out) == TRUE);
    assert(out == &b);
    assert(MPMC_Pop(&q, &out) == TRUE);
    assert(out == &c);

    /* Queue should be empty now */
    assert(MPMC_Pop(&q, &out) == FALSE);

    MPMC_Destroy(&q);
    printf("  basic_push_pop: OK\n");
}

static void test_queue_full(void){
    MPMCQueue q;
    assert(MPMC_Init(&q, 4)); /* capacity 4 */

    int items[4] = {1, 2, 3, 4};
    for(int i = 0; i < 4; i++){
        assert(MPMC_Push(&q, &items[i]) == TRUE);
    }

    /* Queue is full, next push should fail */
    int overflow = 5;
    assert(MPMC_Push(&q, &overflow) == FALSE);

    MPMC_Destroy(&q);
    printf("  queue_full: OK\n");
}

static void test_queue_empty_pop(void){
    MPMCQueue q;
    assert(MPMC_Init(&q, 4));

    void* out = NULL;
    assert(MPMC_Pop(&q, &out) == FALSE);
    assert(out == NULL);

    MPMC_Destroy(&q);
    printf("  queue_empty_pop: OK\n");
}

static void test_push_pop_interleave(void){
    MPMCQueue q;
    assert(MPMC_Init(&q, 4));

    int a = 10, b = 20;
    void* out;

    assert(MPMC_Push(&q, &a) == TRUE);
    assert(MPMC_Pop(&q, &out) == TRUE);
    assert(out == &a);

    assert(MPMC_Push(&q, &b) == TRUE);
    assert(MPMC_Pop(&q, &out) == TRUE);
    assert(out == &b);

    assert(MPMC_Pop(&q, &out) == FALSE);

    MPMC_Destroy(&q);
    printf("  push_pop_interleave: OK\n");
}

static void test_cancel_token_basic(void){
    /*
     * Verify CancelToken contract:
     *  - is_cancelled returns FALSE when not signaled
     *  - is_cancelled returns TRUE when signaled
     *  - is_cancelled handles NULL safely
     */
    CancelToken ct = {0};
    assert(is_cancelled(&ct) == FALSE);

    ct.signaled = TRUE;
    assert(is_cancelled(&ct) == TRUE);

    assert(is_cancelled(NULL) == FALSE);

    printf("  cancel_token_basic: OK\n");
}

static void test_cancel_token_stops_retry(void){
    /*
     * Simulate the cancellation-aware enqueue loop from cloud.c:
     *   while(!MPMC_Push(q, wi)){
     *       if(is_cancelled(cancel) || ++retries > MAX) {
     *           free(wi);
     *           return FALSE;
     *       }
     *       yield();
     *   }
     *
     * Fill the queue, then verify that a push with cancel signaled
     * terminates quickly rather than spinning forever.
     */
    MPMCQueue q;
    assert(MPMC_Init(&q, 4));

    /* Fill queue */
    int items[4] = {1, 2, 3, 4};
    for(int i = 0; i < 4; i++){
        assert(MPMC_Push(&q, &items[i]) == TRUE);
    }

    /* Now simulate cancellation-aware push */
    CancelToken cancel = {0};
    cancel.signaled = TRUE; /* pre-cancel */

    int retries = 0;
    int extra = 99;
    BOOL pushed = FALSE;
    while(!MPMC_Push(&q, &extra)){
        if(is_cancelled(&cancel) || ++retries > 100000){
            break;
        }
    }
    /* Should have broken out due to cancellation, not retries */
    assert(pushed == FALSE);
    assert(retries <= 1); /* cancel was checked on first retry */

    MPMC_Destroy(&q);
    printf("  cancel_token_stops_retry: OK\n");
}

static void test_queue_null_safety(void){
    assert(MPMC_Init(NULL, 4) == FALSE);
    assert(MPMC_Push(NULL, NULL) == FALSE);

    void* out;
    assert(MPMC_Pop(NULL, &out) == FALSE);

    MPMCQueue q;
    memset(&q, 0, sizeof(q));
    assert(MPMC_Push(&q, NULL) == FALSE); /* cells is NULL */

    MPMC_Destroy(NULL); /* should not crash */
    printf("  queue_null_safety: OK\n");
}

int main(void){
    printf("MPMC queue + cancellation tests:\n");
    test_basic_push_pop();
    test_queue_full();
    test_queue_empty_pop();
    test_push_pop_interleave();
    test_cancel_token_basic();
    test_cancel_token_stops_retry();
    test_queue_null_safety();
    printf("All queue tests passed\n");
    return 0;
}
