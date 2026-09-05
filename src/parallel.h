#ifndef PARALLEL_H
#define PARALLEL_H

#define N148_MAX_WORKERS 32

// Calls body(start, end, index, context) over [0, total) split into
// contiguous chunks, one per worker.
typedef void (*ParallelBody)(long start, long end, int worker, void *context);
void n148_parallel_for(long total, ParallelBody body, void *context);

#endif
