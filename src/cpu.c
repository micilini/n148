#include "cpu.h"

#if defined(__x86_64__) || defined(__i386__)
#include <stddef.h>
#include <cpuid.h>

static void cpuid_count(unsigned int leaf, unsigned int subleaf,
                        unsigned int registers[4]) {
    __cpuid_count(leaf, subleaf,
                  registers[0], registers[1], registers[2], registers[3]);
}

// The OS must also agree to save the wide registers on a context
// switch. Checking CPUID alone is not enough: xgetbv tells us whether
// the operating system has enabled AVX state saving.
static int os_supports_avx(void) {
    unsigned int eax;
    unsigned int edx;
    __asm__ volatile (".byte 0x0f, 0x01, 0xd0"
                      : "=a"(eax), "=d"(edx) : "c"(0));
    unsigned long long xcr = ((unsigned long long)edx << 32) | eax;
    return (xcr & 0x6) == 0x6;
}

static int detect(void) {
    unsigned int max_leaf = __get_cpuid_max(0, NULL);
    if (max_leaf < 1) {
        return N148_CPU_BASELINE;
    }

    unsigned int registers[4];
    cpuid_count(1, 0, registers);
    int has_sse2 = (int)((registers[3] >> 26) & 1);
    int has_fma = (int)((registers[2] >> 12) & 1);
    int has_osxsave = (int)((registers[2] >> 27) & 1);
    int has_avx = (int)((registers[2] >> 28) & 1);

    int has_avx2 = 0;
    if (max_leaf >= 7) {
        cpuid_count(7, 0, registers);
        has_avx2 = (int)((registers[1] >> 5) & 1);
    }

    if (has_avx2 && has_avx && has_osxsave && os_supports_avx()) {
        return has_fma ? N148_CPU_AVX2_FMA : N148_CPU_AVX2;
    }
    if (has_sse2) {
        return N148_CPU_SSE2;
    }
    return N148_CPU_BASELINE;
}
#else
static int detect(void) {
    return N148_CPU_BASELINE;
}
#endif

static int detected = -1;
static int forced = -1;

int n148_cpu_level(void) {
    if (forced >= 0) {
        return forced;
    }
    if (detected < 0) {
        detected = detect();
    }
    return detected;
}

void n148_cpu_force(int level) {
    forced = level;
}
