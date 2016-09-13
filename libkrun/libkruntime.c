/*
 * C support functions for timing.
 *
 * Note that this library contains explicit calls to exit() upon error
 * conditions.
 *
 * This file assumes you are using an x86 system.
 *
 * Code style is KNF with 4 spaces instead of tabs.
 */
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#include "libkruntime.h"

#ifdef WITH_JAVA
#include <jni.h>
#endif

#if defined(__linux__)
#define ACTUAL_CLOCK_MONOTONIC    CLOCK_MONOTONIC_RAW
#else
#define ACTUAL_CLOCK_MONOTONIC    CLOCK_MONOTONIC
#endif

#if defined(__linux__) && !defined(TRAVIS)
/*
 * Rather than open and close the MSR device nodes all the time, we hold them
 * open over multiple in-process iterations, thus minimising the amount of work
 * that needs to be done to use them
 */
int *msr_w_nodes = NULL;
int *msr_r_nodes = NULL;

int num_msr_nodes = -1;
#define MAX_MSR_PATH 32

// Fixed-funtion counter control register
#define MSR_IA32_FIXED_CTR_CTRL 0x38d

/*
 * Bitfields of MSR_IA32_FIXED_CTR_CTRL related to fixed counter 1.
 * AKA, CPU_CLK_UNHLATED.CORE in the Intel manual.
 */
// Enable couting in ring 0
#define EN1_OS      1 << 4
// Enable counting in higer rings
#define EN1_USR     1 << 5
// Enable counting for all core threads (if any)
#define EN1_ANYTHR  1 << 6

/* MSR address of fixed-function performance counter 1 */
#define PCTR_IA32_PERF_FIXED_CTR1    0x30a
uint64_t pctr_val_mask = 0; // configured in initialisation

#elif defined(__linux__) && defined(TRAVIS)
// Travis has no performance counters.
#elif defined(__OpenBSD__)
// We do not yet support performance counters on OpenBSD
#else
#error "Unsupported platform"
#endif // __linux__ && !TRAVIS

double
clock_gettime_monotonic()
{
    struct timespec         ts;
    double                  result;

    if ((clock_gettime(ACTUAL_CLOCK_MONOTONIC, &ts)) < 0) {
        perror("clock_gettime");
        exit(1);
    }

    result = ts.tv_sec + ts.tv_nsec * 1e-9;
    return (result);
}

/*
 * JNI wrappers -- Optionally compiled in
 */
#ifdef WITH_JAVA
JNIEXPORT void JNICALL
Java_IterationsRunner_JNI_1libkruntime_1init(JNIEnv *e, jclass c) {
    libkruntime_init();
}

JNIEXPORT void JNICALL
Java_IterationsRunner_JNI_1libkruntime_1done(JNIEnv *e, jclass c) {
    libkruntime_done();
}

JNIEXPORT jdouble JNICALL
Java_IterationsRunner_JNI_1clock_1gettime_1monotonic(JNIEnv *e, jclass c) {
    return (jdouble) clock_gettime_monotonic();
}

JNIEXPORT jlong JNICALL
Java_IterationsRunner_JNI_1read_1core_1cycles(JNIEnv *e, jclass c)
{
    return read_core_cycles();
}
#endif

#if defined(__linux__) && !defined(TRAVIS)
int
open_msr_node(int core, int flags)
{
    char path[MAX_MSR_PATH];
    int msr_node;

    /*
     * Note this is not the default msr(4) device node!
     *
     * We are using a lightly modified version of that driver we call rmsr,
     * which disables capabilities on the device node. This allows a normal
     * user to access the device as per normal filesystem permissions, without
     * having to tag executables with capabilities, and whilst retaining the
     * use of LD_LIBRARY_PATH (which Krun uses a lot).
     *
     * https://github.com/softdevteam/rmsr
     */
    if (snprintf(path, MAX_MSR_PATH, "/dev/cpu/%d/rmsr", core) < 0) {
        perror("snprintf");
        exit(EXIT_FAILURE);
    }

    msr_node = open(path, flags);
    if (msr_node == -1) {
        perror(path);
        exit(EXIT_FAILURE);
    }

    return msr_node;
}

u_int64_t
read_msr(int core, long addr)
{
    u_int64_t msr_val;
    int msr_node;

    msr_node = msr_r_nodes[core];

    if (lseek(msr_node, addr, SEEK_SET) == -1) {
        perror("lseek");
        exit(EXIT_FAILURE);
    }

    if (read(msr_node, &msr_val, sizeof(msr_val)) != sizeof(msr_val)) {
        perror("read");
        exit(EXIT_FAILURE);
    }

    return msr_val;
}

void
write_msr(int core, long addr, uint64_t msr_val)
{
    int msr_node;

    msr_node = msr_w_nodes[core];

    if (lseek(msr_node, addr, SEEK_SET) == -1) {
        perror("lseek");
        exit(EXIT_FAILURE);
    }

    if (write(msr_node, &msr_val, sizeof(msr_val)) != sizeof(msr_val)) {
        perror("write");
        exit(EXIT_FAILURE);
    }
}

/*
 * Configure fixed-function counter 1 to count all rings and threads
 */
void
config_fixed_ctr1(int core, int enable)
{
    u_int64_t msr_val = read_msr(core, MSR_IA32_FIXED_CTR_CTRL);
    if (enable) {
        msr_val |= (EN1_OS | EN1_USR | EN1_ANYTHR);
    } else {
        msr_val &= ~(EN1_OS | EN1_USR | EN1_ANYTHR);
    }
    write_msr(core, MSR_IA32_FIXED_CTR_CTRL, msr_val);
}

void
config_fixed_ctr1_all_cores(int enable)
{
    int core;

    for (core = 0; core < num_msr_nodes; core++) {
        config_fixed_ctr1(core, enable);
        write_msr(core, PCTR_IA32_PERF_FIXED_CTR1, 0); // reset
    }
}

void
close_fd(int fd)
{
    int ok = close(fd);
    if (ok == -1) {
        perror("close");
        exit(EXIT_FAILURE);
    }
}

int
get_fixed_pctr1_width()
{
    uint32_t eax, edx;
    int fixed_ctr_width, arch_ctr_vers, num_fixed_ctrs;

    asm volatile(
        "mov $0xa, %%eax\n\t"       // pctr leaf
        "cpuid\n\t"
        : "=a" (eax), "=d" (edx)    // out
        :                           // in
        : "ebx", "ecx");            // clobber

    /* eax
     *   0-4:  number of fixed-func counters
     *   5-12: width of counters
     */
    num_fixed_ctrs = edx & 0x1f;
    fixed_ctr_width = (edx & 0x1fe0) >> 5;

    /* eax
     *   0-7:  architectural counter version
     *   8-31: reserved
     */
    arch_ctr_vers = eax & 0xff;

    // fixed function perf ctrs appeared on arch counter version 2
    if (arch_ctr_vers < 2) {
        printf("arch pctr version >=2 is required! got %d\n", arch_ctr_vers);
        exit(EXIT_FAILURE);
    }

    // We are interested in IA32_FIXED_CTR1, (i.e. the second fixed counter)
    if (num_fixed_ctrs < 2) {
        fprintf(stderr, "too few fixed-function counters: %d\n",
            num_fixed_ctrs);
        exit(EXIT_FAILURE);
    }

    return fixed_ctr_width;
}
#elif defined(__linux__) && defined(TRAVIS)
// Travis has no performance counters
#elif defined(__OpenBSD__)
// We do not yet support performance counters on OpenBSD
#else
#error "Unsupported platform"
#endif // linux && !TRAVIS

void
libkruntime_init(void)
{
#if defined(__linux__) && !defined(TRAVIS)
    int core;

    /* See how wide the counter values are and make an appropriate mask */
    pctr_val_mask = ((uint64_t) 1 << get_fixed_pctr1_width()) - 1;

    /* Set up MSR device node handles */
    num_msr_nodes = sysconf(_SC_NPROCESSORS_ONLN);

    msr_r_nodes = calloc(num_msr_nodes, sizeof(int));
    if (msr_r_nodes == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    msr_w_nodes = calloc(num_msr_nodes, sizeof(int));
    if (msr_w_nodes == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (core = 0; core < num_msr_nodes; core++) {
        msr_r_nodes[core] = open_msr_node(core, O_RDONLY);
        msr_w_nodes[core] = open_msr_node(core, O_WRONLY);
    }

    /* Configure and reset CPU_CLK_UNHALTED.CORE on all CPUs */
    config_fixed_ctr1_all_cores(1);
#elif defined(__linux__) && defined(TRAVIS)
    // Travis has no performance counters
#elif defined(__OpenBSD__)
    // We do not yet support performance counters on OpenBSD
#else
#   error "Unsupported platform"
#endif  // __linux__ && !TRAVIS
}

void
libkruntime_done(void)
{
#if defined(__linux__) && !defined(TRAVIS)
    int core;

    /* Close MSR device nodes */
    for (core = 0; core < num_msr_nodes; core++) {
        close_fd(msr_r_nodes[core]);
        close_fd(msr_w_nodes[core]);
    }
#elif defined(__linux__) && defined(TRAVIS)
    // Travis has no performance counters
#elif defined(__OpenBSD__)
    // We do not yet support performance counters on OpenBSD
#else
#error "Unsupported platform"
#endif  // __linux__ && !TRAVIS
}

u_int64_t
read_core_cycles(void)
{
#if defined(__linux__) && !defined(TRAVIS)
    int core;
    uint64_t cycles = 0, val;

    for (core = 0; core < num_msr_nodes; core++) {
        val = read_msr(core, PCTR_IA32_PERF_FIXED_CTR1) & pctr_val_mask;

        /*
         * Note that overflow is impossible on most platforms. E.g. on an i7,
         * the pctr values are 42-bit, and we sum into a 64-bit unsigned. In
         * such a configuration you would need a huge number of cores in order
         * to expose the possibility of an overflow.
         *
         * Nevertheless, we check.
         */
        if (cycles > UINT64_MAX - val) {
            fprintf(stderr, "Core cycle count overflow!: "
                    "%" PRIu64 " + %" PRIu64 "\n", cycles, val);
            exit(EXIT_FAILURE);
        }
        cycles += val;
    }

    return cycles;
#elif defined(__linux__) && defined(TRAVIS)
    // Travis has no performance counters
    return 0;
#elif defined(__OpenBSD__)
    // We do not yet support performance counters on OpenBSD
    return 0;
#else
#error "Unsupported platform"
#endif // __linux__ && !TRAVIS
}

/*
 * For languages like Lua, where there is suitible integer type
 */
double
read_core_cycles_double()
{
    return u64_to_double(read_core_cycles());
}

/*
 * Check for double precision loss.
 *
 * Since some languages cannot represent a u_int64_t, we sometimes have to pass
 * around a double. This is annoying since precision could be silently lost.
 * This function makes loss of precision explicit, stopping the VM.
 *
 * We don't expect to actually see a crash since the TSR is zeroed at reboot,
 * and our benchmarks are not long enough running to generate a large enough
 * TSR value to cause precision loss (you would need a integer that would not
 * fit in a 52-bit unsigned int before precision starts being lost in
 * lower-order bits). Nevertheless, we check it.
 *
 * This routine comes at a small cost (a handful of asm instrs). Note that this
 * cost is a drop in the ocean compared to benchmark workloads.
 */
double
u64_to_double(u_int64_t u64_val)
{
    double d_val = (double) u64_val;
    u_int64_t u64_val2 = (u_int64_t) d_val;

    if (u64_val != u64_val2) {
        fprintf(stderr, "Loss of precision detected!\n");
        fprintf(stderr, "%" PRIu64 " != %" PRIu64 "\n", u64_val, u64_val2);
        exit(EXIT_FAILURE);
    }
    // (otherwise all is well)
    return d_val;
}
