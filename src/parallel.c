#include <pthread.h>
#include <unistd.h>

#include "parallel.h"

#ifndef N148_WORKERS
#define N148_WORKERS 0
#endif

typedef struct {
    ParallelBody body;
    void *context;
    long start;
    long end;
    int index;
} WorkerArgs;

static void *worker_entry(void *opaque) {
    WorkerArgs *args = (WorkerArgs *)opaque;
    args->body(args->start, args->end, args->index, args->context);
    return NULL;
}

static int available_workers(void) {
#if N148_WORKERS > 0
    int workers = N148_WORKERS;
#else
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = online > 0 ? (int)online : 1;
#endif
    if (workers > N148_MAX_WORKERS) {
        workers = N148_MAX_WORKERS;
    }
    return workers > 0 ? workers : 1;
}

void n148_parallel_for(long total, ParallelBody body, void *context) {
    if (!body || total <= 0) {
        return;
    }

    int workers = available_workers();

    // Very small jobs are not worth the handover cost.
    if (workers <= 1 || total < 8) {
        body(0, total, 0, context);
        return;
    }

    long chunk = total / workers + (total % workers != 0);
    pthread_t threads[N148_MAX_WORKERS];
    WorkerArgs args[N148_MAX_WORKERS];
    unsigned char created[N148_MAX_WORKERS] = {0};

    for (int i = 1; i < workers; i++) {
        long start = chunk * i;
        if (start >= total) {
            break;
        }
        long end = start + chunk;
        if (end > total) {
            end = total;
        }

        args[i].body = body;
        args[i].context = context;
        args[i].start = start;
        args[i].end = end;
        args[i].index = i;

        if (pthread_create(&threads[i], NULL, worker_entry, &args[i]) == 0) {
            created[i] = 1;
        } else {
            body(start, end, i, context);
        }
    }

    // The calling thread takes the first slice instead of idling.
    long first_end = chunk < total ? chunk : total;
    body(0, first_end, 0, context);

    for (int i = 1; i < workers; i++) {
        if (created[i]) {
            pthread_join(threads[i], NULL);
        }
    }
}
