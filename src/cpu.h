/*
 * N.148 codec - runtime CPU feature detection.
 *
 * The codec ships several implementations of its hot loops: a portable
 * scalar one that runs anywhere, and hand-tuned SIMD ones. Which path
 * runs is decided ONCE at startup by asking the CPU what it supports,
 * so a single binary stays fast on modern machines and correct on old
 * ones.
 */
#ifndef CPU_H
#define CPU_H

#define N148_CPU_BASELINE 0
#define N148_CPU_SSE2     1
#define N148_CPU_AVX2     2
#define N148_CPU_AVX2_FMA 3

// Detects the CPU once and caches the answer.
int n148_cpu_level(void);

// Human readable name of the active path, for reporting.
const char *n148_cpu_name(void);

// Forces a level (used by the test suite to compare paths).
void n148_cpu_force(int level);

#endif
