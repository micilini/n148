/*
 * N.148 codec - a small parallel helper.
 *
 * Several stages of the codec are embarrassingly parallel: colour
 * conversion, the forward transform and the final reconstruction all
 * treat rows (or rows of blocks) independently. This helper splits a
 * range across worker threads and waits for them, without pulling in
 * a full thread pool.
 *
 * Build without N148_THREADS defined, or set the thread count to 1,
 * and everything runs inline on the calling thread.
 */
#ifndef PARALLEL_H
#define PARALLEL_H

// Number of workers the codec will use. Defaults to the number of
// cores reported by the system, clamped to a sane maximum.
int  n148_thread_count(void);
void n148_set_thread_count(int count);

// Calls body(start, end, index, context) over [0, total) split into
// contiguous chunks, one per worker.
typedef void (*ParallelBody)(long start, long end, int worker, void *context);
void n148_parallel_for(long total, ParallelBody body, void *context);

#endif
