#include <stdlib.h>
#include "parallel.h"

#define N148_MAX_THREADS 32

#if defined(_WIN32)
#include <windows.h>
#elif !defined(N148_NO_THREADS) && (defined(__unix__) || defined(__APPLE__))
#include <pthread.h>
#include <unistd.h>
#define N148_HAVE_PTHREADS 1
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

static int configured = 0;

static int detect_cores(void) {
#if defined(N148_NO_THREADS)
    return 1;
#elif defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int) info.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int) count : 1;
#else
    return 1;
#endif
}

int n148_thread_count(void) {
    if (configured == 0) {
        configured = detect_cores();
        if (configured < 1) configured = 1;
        if (configured > N148_MAX_THREADS) configured = N148_MAX_THREADS;
    }
    return configured;
}

void n148_set_thread_count(int count) {
    if (count < 1) count = 1;
    if (count > N148_MAX_THREADS) count = N148_MAX_THREADS;
#ifdef N148_NO_THREADS
    count = 1;
#endif
    configured = count;
}

#ifdef N148_HAVE_PTHREADS
typedef struct {
    int index;
    unsigned long generation;
} WorkerSlot;

typedef struct {
    pthread_mutex_t state_lock;
    pthread_mutex_t dispatch_lock;
    pthread_cond_t work_ready;
    pthread_cond_t work_done;
    pthread_t threads[N148_MAX_THREADS - 1];
    WorkerSlot slots[N148_MAX_THREADS - 1];
    int initialized;
    int capacity;
    int participants;
    int remaining;
    int stopping;
    unsigned long generation;
    long total;
    ParallelBody body;
    void *context;
} ThreadPool;

static ThreadPool pool = {
    .state_lock = PTHREAD_MUTEX_INITIALIZER,
    .dispatch_lock = PTHREAD_MUTEX_INITIALIZER,
    .work_ready = PTHREAD_COND_INITIALIZER,
    .work_done = PTHREAD_COND_INITIALIZER
};

static void *pool_worker(void *raw) {
    WorkerSlot *slot = (WorkerSlot *) raw;
    pthread_mutex_lock(&pool.state_lock);

    for (;;) {
        while (!pool.stopping && slot->generation == pool.generation)
            pthread_cond_wait(&pool.work_ready, &pool.state_lock);
        if (pool.stopping) break;

        slot->generation = pool.generation;
        int index = slot->index;
        if (index >= pool.participants) continue;

        long total = pool.total;
        int participants = pool.participants;
        ParallelBody body = pool.body;
        void *context = pool.context;
        long start = total * index / participants;
        long end = total * (index + 1) / participants;

        pthread_mutex_unlock(&pool.state_lock);
        body(start, end, index, context);
        pthread_mutex_lock(&pool.state_lock);

        if (--pool.remaining == 0)
            pthread_cond_signal(&pool.work_done);
    }

    pthread_mutex_unlock(&pool.state_lock);
    return NULL;
}

static void pool_shutdown(void) {
    pthread_mutex_lock(&pool.dispatch_lock);
    if (!pool.initialized) {
        pthread_mutex_unlock(&pool.dispatch_lock);
        return;
    }

    pthread_mutex_lock(&pool.state_lock);
    pool.stopping = 1;
    pool.generation++;
    pthread_cond_broadcast(&pool.work_ready);
    pthread_mutex_unlock(&pool.state_lock);

    for (int i = 0; i < pool.capacity; i++)
        pthread_join(pool.threads[i], NULL);
    pool.initialized = 0;
    pool.capacity = 0;
    pthread_mutex_unlock(&pool.dispatch_lock);
}

/* Called with dispatch_lock held. */
static void pool_initialize(int background_workers) {
    if (pool.initialized) return;

    pool.initialized = 1;
    pool.capacity = 0;
    pool.stopping = 0;
    pool.generation = 0;
    if (background_workers > N148_MAX_THREADS - 1)
        background_workers = N148_MAX_THREADS - 1;

    for (int i = 0; i < background_workers; i++) {
        WorkerSlot *slot = &pool.slots[i];
        slot->index = i + 1;
        slot->generation = 0;
        if (pthread_create(&pool.threads[i], NULL, pool_worker, slot) != 0)
            break;
        pool.capacity++;
    }
    atexit(pool_shutdown);
}
#endif

void n148_parallel_for(long total, ParallelBody body, void *context) {
    if (total <= 0) return;

    int workers = n148_thread_count();
    if (workers <= 1 || total < 8) {
        body(0, total, 0, context);
        return;
    }
    if (workers > total) workers = (int) total;

#ifdef N148_HAVE_PTHREADS
    pthread_mutex_lock(&pool.dispatch_lock);
    pool_initialize(workers - 1);
    if (workers > pool.capacity + 1) workers = pool.capacity + 1;
    if (workers <= 1) {
        pthread_mutex_unlock(&pool.dispatch_lock);
        body(0, total, 0, context);
        return;
    }

    pthread_mutex_lock(&pool.state_lock);
    pool.total = total;
    pool.participants = workers;
    pool.remaining = workers - 1;
    pool.body = body;
    pool.context = context;
    pool.generation++;
    pthread_cond_broadcast(&pool.work_ready);
    pthread_mutex_unlock(&pool.state_lock);

    body(0, total / workers, 0, context);

    pthread_mutex_lock(&pool.state_lock);
    while (pool.remaining != 0)
        pthread_cond_wait(&pool.work_done, &pool.state_lock);
    pthread_mutex_unlock(&pool.state_lock);
    pthread_mutex_unlock(&pool.dispatch_lock);
#else
    body(0, total, 0, context);
#endif
}
