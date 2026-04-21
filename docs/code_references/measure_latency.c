/*
 * measure_latency.c — measure raw CACHELINE_STORE / CACHELINE_LOAD latency
 * on whatever memory the mmap'd file is backed by (tmpfs / real CXL).
 *
 * Usage: ./measure_latency [--path=/dev/shm/mlat.bin]
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#include "../common.h"

#define REGION_SIZE (16 * 1024 * 1024)  /* 16 MB */
#define N_CACHELINES (REGION_SIZE / CACHE_LINE_SIZE)
#define ITERS 200000

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    const char *path = "/dev/shm/mlat.bin";
    static struct option longopts[] = {
        {"path", required_argument, 0, 'p'}, {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:", longopts, NULL)) != -1) {
        if (c == 'p') path = optarg;
    }

    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { perror("open"); return 1; }
    /* ftruncate will fail on char devdax devices; ignore if it does */
    if (ftruncate(fd, REGION_SIZE) < 0 && errno != EINVAL) {
        perror("ftruncate"); return 1;
    }

    cacheline_u64 *arr = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    if (arr == MAP_FAILED) { perror("mmap"); return 1; }

    /* Pre-touch to avoid page-fault in measurement */
    memset(arr, 0, REGION_SIZE);
    flush_region(arr, REGION_SIZE);

    printf("Measuring on backing: %s (REGION=%d MB, N=%d iterations)\n",
           path, REGION_SIZE / (1024*1024), ITERS);

    /* --- CACHELINE_STORE --- */
    uint64_t t0 = now_ns();
    for (int i = 0; i < ITERS; i++) {
        CACHELINE_STORE(&arr[i % N_CACHELINES], (uint64_t)i);
    }
    uint64_t t1 = now_ns();
    double store_ns = (double)(t1 - t0) / ITERS;

    /* --- CACHELINE_LOAD --- */
    t0 = now_ns();
    volatile uint64_t sink = 0;
    for (int i = 0; i < ITERS; i++) {
        sink += CACHELINE_LOAD(&arr[i % N_CACHELINES]);
    }
    t1 = now_ns();
    double load_ns = (double)(t1 - t0) / ITERS;
    (void)sink;

    /* --- Plain store (cached) --- */
    t0 = now_ns();
    for (int i = 0; i < ITERS; i++) {
        arr[i % N_CACHELINES].value = (uint64_t)i;
    }
    t1 = now_ns();
    double plain_store_ns = (double)(t1 - t0) / ITERS;

    /* --- Plain load (cached) --- */
    t0 = now_ns();
    sink = 0;
    for (int i = 0; i < ITERS; i++) {
        sink += arr[i % N_CACHELINES].value;
    }
    t1 = now_ns();
    double plain_load_ns = (double)(t1 - t0) / ITERS;
    (void)sink;

    printf("\nResults (mean):\n");
    printf("  plain store    (no flush):       %.1f ns\n", plain_store_ns);
    printf("  plain load     (cached):         %.1f ns\n", plain_load_ns);
    printf("  CACHELINE_STORE (+ flush+sfence): %.1f ns\n", store_ns);
    printf("  CACHELINE_LOAD  (+ flush+mfence): %.1f ns\n", load_ns);

    munmap(arr, REGION_SIZE);
    close(fd);
    unlink(path);
    return 0;
}
