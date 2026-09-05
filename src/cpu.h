#ifndef CPU_H
#define CPU_H

#define N148_CPU_BASELINE 0
#define N148_CPU_SSE2 1
#define N148_CPU_AVX2 2

// Returns the best SIMD level supported by both the CPU and the OS.
int n148_cpu_level(void);

// Overrides runtime detection for tests. Pass a negative value to reset it.
void n148_cpu_force(int level);

#endif
