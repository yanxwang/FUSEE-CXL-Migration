// iter-19A Phase 1c — FORKED synthetic to test H7 directly.
// Same access pattern + entry layout as iter19A_phase1b_synthetic_cache_pool_bench.c
// but uses fork() to create N worker processes (like FUSEE), instead of pthreads.
// Shared cache_pool via MAP_SHARED|MAP_ANONYMOUS.
//
// If forked synth shows Anomaly-A-like scaling collapse at high T → fork
// model is THE culprit. If forked synth still scales linearly → something
// FUSEE-specific (search() wrapper, spec_trans walk, ring init, etc.).
//
// Usage: ./synth_fork <num_buckets> <num_procs> <ops_per_proc>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>
#include <math.h>

#define ENTRY_VALUE_BYTES 1024
#define ENTRIES_PER_BUCKET 4

struct __attribute__((aligned(64))) Entry {
    _Atomic uint64_t key;
    _Atomic uint8_t stale;
    uint8_t pad_a[3];
    uint32_t value_size;
    _Atomic uint64_t lru_epoch;
    _Atomic uint32_t seq;
    uint32_t pad_seq;
    uint8_t value_bytes[ENTRY_VALUE_BYTES];
    uint8_t pad_tail[28];
};
struct __attribute__((aligned(64))) Bucket {
    struct Entry entries[ENTRIES_PER_BUCKET];
    _Atomic uint64_t epoch;
    uint8_t pad[56];
};
struct __attribute__((aligned(64))) WorkerOut {
    uint64_t completed;
    uint64_t total_ns;
};

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static inline uint64_t xs64(uint64_t *st) {
    uint64_t x = *st; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *st = x; return x;
}
static void gen_zipf(uint32_t *pool, uint64_t pool_size, uint64_t key_range, double s) {
    double H = 0.0;
    for (uint64_t i = 1; i <= key_range; i++) H += 1.0 / pow((double)i, s);
    double *cum = malloc(sizeof(double) * key_range);
    double c = 0.0;
    for (uint64_t i = 0; i < key_range; i++) {
        c += 1.0 / pow((double)(i+1), s);
        cum[i] = c / H;
    }
    uint64_t st = 0xdeadbeefULL ^ pool_size;
    for (uint64_t i = 0; i < pool_size; i++) {
        double u = (double)(xs64(&st) >> 11) * (1.0 / (double)(1ull<<53));
        uint64_t lo = 0, hi = key_range - 1;
        while (lo < hi) { uint64_t m = (lo+hi)/2; if (cum[m] < u) lo = m+1; else hi = m; }
        pool[i] = (uint32_t)lo;
    }
    free(cum);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <num_buckets> <num_procs> <ops_per_proc>\n", argv[0]); return 1; }
    uint64_t num_buckets = strtoull(argv[1], 0, 0);
    int P = atoi(argv[2]);
    uint64_t ops_per_proc = strtoull(argv[3], 0, 0);

    uint64_t cache_bytes = num_buckets * sizeof(struct Bucket);
    fprintf(stderr, "[synth_fork] buckets=%lu procs=%d ops/proc=%lu cache=%.1f MiB\n",
            num_buckets, P, ops_per_proc, cache_bytes / (1024.0*1024.0));

    // SHARED cache_pool (MAP_SHARED matches FUSEE)
    struct Bucket *buckets = mmap(NULL, cache_bytes, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (buckets == MAP_FAILED) { perror("mmap cache"); return 1; }
    madvise(buckets, cache_bytes, MADV_HUGEPAGE);

    // Touch + init keys
    for (uint64_t i = 0; i < num_buckets; i++) {
        for (int e = 0; e < ENTRIES_PER_BUCKET; e++) {
            atomic_store(&buckets[i].entries[e].key, i * ENTRIES_PER_BUCKET + e);
            buckets[i].entries[e].value_size = ENTRY_VALUE_BYTES;
            buckets[i].entries[e].value_bytes[0] = (uint8_t)i;
        }
    }

    _Atomic uint64_t *global_epoch = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    atomic_store(global_epoch, 1);

    // Zipf indices (shared via mmap so children see)
    uint64_t pool_size = 2 * 1024 * 1024;
    uint32_t *zipf = mmap(NULL, sizeof(uint32_t) * pool_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint64_t key_range = num_buckets > 1000000 ? 1000000 : num_buckets;
    gen_zipf(zipf, pool_size, key_range, 0.99);

    // Per-process result slots
    struct WorkerOut *outs = mmap(NULL, sizeof(struct WorkerOut) * P, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t t0 = now_ns();
    for (int p = 0; p < P; p++) {
        pid_t pid = fork();
        if (pid == 0) {
            // child
            cpu_set_t s; CPU_ZERO(&s); CPU_SET(p % ncpu, &s);
            sched_setaffinity(0, sizeof(s), &s);

            uint64_t st = 0xc0ffee00ULL ^ ((uint64_t)p << 32);
            uint8_t local_value[ENTRY_VALUE_BYTES];
            uint64_t c_t0 = now_ns();
            uint64_t ok = 0;
            for (uint64_t i = 0; i < ops_per_proc; i++) {
                uint32_t idx = zipf[xs64(&st) % pool_size] % num_buckets;
                struct Bucket *bk = &buckets[idx];
                int e = (int)(xs64(&st) % ENTRIES_PER_BUCKET);
                struct Entry *en = &bk->entries[e];
                uint32_t s1 = atomic_load_explicit(&en->seq, memory_order_acquire);
                (void)s1;
                uint64_t k = atomic_load_explicit(&en->key, memory_order_acquire);
                (void)k;
                memcpy(local_value, en->value_bytes, ENTRY_VALUE_BYTES);
                uint32_t s2 = atomic_load_explicit(&en->seq, memory_order_acquire);
                (void)s2;
                uint64_t ge = atomic_load_explicit(global_epoch, memory_order_relaxed);
                atomic_store_explicit(&en->lru_epoch, ge, memory_order_relaxed);
                ok++;
            }
            uint64_t c_t1 = now_ns();
            outs[p].completed = ok;
            outs[p].total_ns = c_t1 - c_t0;
            _exit(0);
        }
    }
    for (int p = 0; p < P; p++) {
        int st; wait(&st);
    }
    uint64_t t1 = now_ns();

    uint64_t total_ops = 0, total_ns_sum = 0;
    for (int p = 0; p < P; p++) {
        total_ops += outs[p].completed;
        total_ns_sum += outs[p].total_ns;
    }
    double wall_s = (t1 - t0) / 1e9;
    double aggr_mops = total_ops / wall_s / 1e6;
    double per_op_ns = (double)total_ns_sum / total_ops;
    printf("SYNTH_FORK buckets=%lu P=%d total_ops=%lu wall=%.3fs aggr_thpt=%.2f Mops per_op=%.1f ns ws_MiB=%.1f\n",
           num_buckets, P, total_ops, wall_s, aggr_mops, per_op_ns,
           cache_bytes / (1024.0*1024.0));
    return 0;
}
