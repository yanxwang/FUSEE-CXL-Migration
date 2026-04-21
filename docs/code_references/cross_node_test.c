/*
 * cross_node_test.c
 * Verify two nodes see the same physical CXL memory.
 *
 *   Node A:  ./cross_node_test --write  --path=/dev/dax0.0 --magic=0xDEADBEEF
 *   Node B:  ./cross_node_test --read   --path=/dev/dax0.0 --magic=0xDEADBEEF
 *
 * If Node B reads the magic value Node A wrote, they share the same memory.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../common.h"

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s --write|--read [--path=/dev/dax0.0] [--magic=0xDEADBEEF] [--poll]\n", p);
    exit(1);
}

int main(int argc, char **argv) {
    int mode = 0;  /* 0=none, 1=write, 2=read */
    const char *path = "/dev/dax0.0";
    uint64_t magic = 0xDEADBEEFCAFEULL;
    int poll_mode = 0;

    static struct option longopts[] = {
        {"write",   no_argument,       0, 'w'},
        {"read",    no_argument,       0, 'r'},
        {"path",    required_argument, 0, 'p'},
        {"magic",   required_argument, 0, 'm'},
        {"poll",    no_argument,       0, 'P'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "wrp:m:P", longopts, NULL)) != -1) {
        switch (c) {
            case 'w': mode = 1; break;
            case 'r': mode = 2; break;
            case 'p': path = optarg; break;
            case 'm': magic = strtoull(optarg, NULL, 0); break;
            case 'P': poll_mode = 1; break;
            default:  usage(argv[0]);
        }
    }
    if (mode == 0) usage(argv[0]);

    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    size_t region = 2 * 1024 * 1024;  /* 2 MB aligned to dax page */
    void *p = mmap(NULL, region, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    cacheline_u64 *test_loc = (cacheline_u64 *)p;

    if (mode == 1) {
        CACHELINE_STORE(test_loc, magic);
        printf("[WRITE] path=%s: wrote magic 0x%lx at offset 0\n", path, magic);
    } else {
        if (poll_mode) {
            printf("[READ ] path=%s: polling for magic 0x%lx ...\n", path, magic);
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t t0 = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
            while (1) {
                uint64_t v = CACHELINE_LOAD(test_loc);
                if (v == magic) {
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    uint64_t t1 = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
                    printf("[READ ] found magic after %.3f ms\n", (t1-t0)/1e6);
                    break;
                }
                usleep(1000);
                if (((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec) - t0 > 30ULL*1000000000ULL) {
                    printf("[READ ] TIMEOUT - got 0x%lx, wanted 0x%lx\n", v, magic);
                    break;
                }
            }
        } else {
            uint64_t v = CACHELINE_LOAD(test_loc);
            if (v == magic) {
                printf("[READ ] path=%s: SUCCESS - found magic 0x%lx\n", path, v);
            } else {
                printf("[READ ] path=%s: MISMATCH - got 0x%lx, wanted 0x%lx\n", path, v, magic);
            }
        }
    }

    munmap(p, region);
    close(fd);
    return 0;
}
