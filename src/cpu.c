#include "cpu.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#define N148_X86 1
#endif

#ifdef N148_X86
#if defined(_MSC_VER)
#include <intrin.h>
static void cpuid_count(int leaf, int sub, int regs[4]) {
    __cpuidex(regs, leaf, sub);
}
#else
#include <cpuid.h>
static void cpuid_count(int leaf, int sub, int regs[4]) {
    __cpuid_count(leaf, sub, regs[0], regs[1], regs[2], regs[3]);
}
#endif

// The OS must also agree to save the wide registers on a context
// switch. Checking CPUID alone is not enough: xgetbv tells us whether
// the operating system has enabled AVX state saving.
static int os_supports_avx(void) {
#if defined(_MSC_VER)
    unsigned long long xcr = _xgetbv(0);
#else
    unsigned int eax, edx;
    __asm__ volatile (".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
    unsigned long long xcr = ((unsigned long long) edx << 32) | eax;
#endif
    return (xcr & 0x6) == 0x6;      // XMM and YMM state enabled
}
#endif

static int detected = -1;
static int forced   = -1;

static int detect(void) {
#ifdef N148_X86
    int regs[4];

    cpuid_count(0, 0, regs);
    int max_leaf = regs[0];

    cpuid_count(1, 0, regs);
    int has_sse2   = (regs[3] >> 26) & 1;
    int has_fma    = (regs[2] >> 12) & 1;
    int has_osxsave= (regs[2] >> 27) & 1;
    int has_avx    = (regs[2] >> 28) & 1;

    int has_avx2 = 0;
    if (max_leaf >= 7) {
        cpuid_count(7, 0, regs);
        has_avx2 = (regs[1] >> 5) & 1;
    }

    if (has_avx2 && has_avx && has_osxsave && os_supports_avx())
        return has_fma ? N148_CPU_AVX2_FMA : N148_CPU_AVX2;
    if (has_sse2)
        return N148_CPU_SSE2;
#endif
    return N148_CPU_BASELINE;
}

int n148_cpu_level(void) {
    if (forced >= 0) return forced;
    if (detected < 0) detected = detect();
    return detected;
}

int n148_cpu_detected_level(void) {
    if (detected < 0) detected = detect();
    return detected;
}

void n148_cpu_force(int level) { forced = level; }

const char *n148_cpu_name(void) {
    switch (n148_cpu_level()) {
        case N148_CPU_AVX2_FMA: return "AVX2+FMA";
        case N148_CPU_AVX2: return "AVX2";
        case N148_CPU_SSE2: return "SSE2";
        default:            return "scalar";
    }
}
