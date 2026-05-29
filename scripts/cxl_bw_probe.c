// Minimal CXL read bandwidth probe.
// mmap a region of /dev/dax0.0, then issue clflushopt+mfence+load on each
// cacheline. This forces every read to go to CXL (no LLC hit), matching
// the FUSEE coherent-read pattern used in production.
//
// Usage:
//   cxl_bw_probe <bytes_per_iter> <num_iters> <offset>
// Defaults: 1 GiB / 3 iters / 0
//
// Output: per-iter and average ns + GB/s.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>

static inline void clflushopt(volatile void *p) {
    __asm__ volatile ("clflushopt (%0)" :: "r"(p) : "memory");
}
static inline void mfence(void) { __asm__ volatile ("mfence" ::: "memory"); }

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    size_t bytes = (argc > 1) ? strtoull(argv[1], 0, 0) : (1ULL << 30);  // 1 GiB
    int iters   = (argc > 2) ? atoi(argv[2]) : 3;
    size_t off  = (argc > 3) ? strtoull(argv[3], 0, 0) : 0;

    int fd = open("/dev/dax0.0", O_RDWR);
    if (fd < 0) { perror("open /dev/dax0.0"); return 1; }
    void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }

    fprintf(stderr, "# mmap /dev/dax0.0 size=%zu off=%zu iters=%d\n",
            bytes, off, iters);
    // Touch each page so we know it's mapped (also "warms up" CXL device).
    volatile uint64_t sink = 0;
    for (size_t i = 0; i < bytes; i += 4096) sink ^= ((volatile uint8_t*)map)[i];

    double total = 0.0;
    for (int it = 0; it < iters; it++) {
        // 1) flush every cacheline so the load definitely goes to CXL
        for (size_t i = 0; i < bytes; i += 64) {
            clflushopt((char*)map + i);
        }
        mfence();
        // 2) measure pure load BW: read every cacheline
        double t0 = now_sec();
        uint64_t acc = 0;
        for (size_t i = 0; i < bytes; i += 64) {
            acc += *(volatile uint64_t*)((char*)map + i);
        }
        mfence();
        double t1 = now_sec();
        double secs = t1 - t0;
        double gbps = (double)bytes / secs / 1e9;
        total += gbps;
        printf("iter %d: %.3f ms  %.2f GB/s  (acc=%lx)\n",
               it, secs * 1000.0, gbps, (unsigned long)acc);
        // prevent compiler from optimizing acc away
        sink ^= acc;
    }
    printf("avg: %.2f GB/s   (%d iters, bytes=%zu)\n",
           total / iters, iters, bytes);
    (void)sink;
    munmap(map, bytes);
    close(fd);
    return 0;
}
