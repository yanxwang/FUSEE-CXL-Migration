// iter-19A Phase 1b — synthetic DRAM bench mimicking FUSEE cache_pool access
//
// Goal: replay the cache_pool_lookup hot path (atomic LRU-touch + memcpy 1024 B)
// over varying working set sizes, with the SAME entry size + access pattern as
// FUSEE production. If thpt drops monotonically with working-set, we confirm
// the Anomaly A residual mechanism is LLC capacity miss (not anything
// FUSEE-coordination-specific).
//
// Per-op work (matches src/cxl_cache_pool.cc:cache_pool_lookup):
//   1) random bucket idx (Zipf-weighted via precomputed indices)
//   2) seq.load(acquire) — 8 B atomic from entry's first cacheline
//   3) key.load(acquire) — 8 B atomic
//   4) memcpy(value_bytes, dst, 1024)
//   5) seq.load(acquire) — re-read for seqlock check
//   6) global_epoch.load(relaxed) — 8 B atomic on shared field
//   7) lru_epoch.store(global_epoch, relaxed) — 8 B atomic on entry
//
// Build: gcc -O2 -pthread -march=native iter19A_phase1b_synthetic_cache_pool_bench.c -o synth
// Usage: ./synth <num_buckets> <num_threads> <ops_per_thread>
//   matching cache_pct: 16384 (c=1), 131072 (c=10), 2097152 (c=100)
//
// All atomics use relaxed/acquire to MATCH production semantics. Working set
// per bucket = sizeof(KvCacheBucket) = 4 * 1088 = 4352 B (4 entries × 1088 B
// 64-byte aligned).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <math.h>

#define ENTRY_VALUE_BYTES 1024
#define ENTRIES_PER_BUCKET 4

// Match KvCacheEntry layout in src/cxl_cache_pool.h: 1088 B per entry
// (8+1+3+4+8+4+4+1024 = 1056, alignas(64) → 1088).
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

struct ThreadArg {
    int tid;
    int cpu;
    uint64_t num_buckets;
    uint64_t ops;
    struct Bucket *buckets;
    _Atomic uint64_t *global_epoch;
    uint32_t *zipf_indices;   // precomputed Zipf hot indices
    uint64_t zipf_pool_size;
    uint64_t completed;
    uint64_t total_ns;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// xorshift64 PRNG
static inline uint64_t xs64(uint64_t *st) {
    uint64_t x = *st;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *st = x;
    return x;
}

// Zipf precomputation: build a pool of indices weighted ~ 1/rank^s.
// Sample uniformly from pool → empirical Zipf draws. With s=0.99 + pool=200M
// the top key gets ~0.5% but density is heavier than expected; we adjust by
// computing CDF with proper Zipf weights.
static void gen_zipf_indices(uint32_t *pool, uint64_t pool_size,
                              uint64_t key_range, double s) {
    // Compute Zipf normalization: H = sum_{i=1..key_range} 1/i^s
    double H = 0.0;
    for (uint64_t i = 1; i <= key_range; i++) H += 1.0 / pow((double)i, s);
    // CDF table: cum[i] = P(rank <= i)
    double *cum = (double*)malloc(sizeof(double) * key_range);
    if (!cum) { fprintf(stderr, "cum alloc fail\n"); exit(1); }
    double c = 0.0;
    for (uint64_t i = 0; i < key_range; i++) {
        c += 1.0 / pow((double)(i+1), s);
        cum[i] = c / H;
    }
    // Sample pool via inverse-CDF (binary search).
    uint64_t st = 0xdeadbeefULL ^ pool_size;
    for (uint64_t i = 0; i < pool_size; i++) {
        double u = (double)(xs64(&st) >> 11) * (1.0 / (double)(1ull<<53));
        // binary search
        uint64_t lo = 0, hi = key_range - 1;
        while (lo < hi) {
            uint64_t m = (lo+hi)/2;
            if (cum[m] < u) lo = m+1; else hi = m;
        }
        pool[i] = (uint32_t)lo;
    }
    free(cum);
}

static void *worker(void *p) {
    struct ThreadArg *a = (struct ThreadArg *)p;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(a->cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);

    uint64_t st = 0xc0ffee00ULL ^ ((uint64_t)a->tid << 32);
    uint8_t local_value[ENTRY_VALUE_BYTES];

    uint64_t t0 = now_ns();
    uint64_t ok = 0;

    for (uint64_t i = 0; i < a->ops; i++) {
        // Pick Zipf-distributed bucket idx
        uint32_t idx = a->zipf_indices[xs64(&st) % a->zipf_pool_size]
                       % a->num_buckets;
        struct Bucket *bk = &a->buckets[idx];
        // Pick a random entry within the bucket
        int e = (int)(xs64(&st) % ENTRIES_PER_BUCKET);
        struct Entry *en = &bk->entries[e];

        // Mimic cache_pool_lookup: seq.load, key.load, memcpy, seq.load
        uint32_t s1 = atomic_load_explicit(&en->seq, memory_order_acquire);
        (void)s1;
        uint64_t k = atomic_load_explicit(&en->key, memory_order_acquire);
        (void)k;
        memcpy(local_value, en->value_bytes, ENTRY_VALUE_BYTES);
        uint32_t s2 = atomic_load_explicit(&en->seq, memory_order_acquire);
        (void)s2;
        // LRU touch (the suspected B-H2 mechanism, kept here for fidelity):
        uint64_t ge = atomic_load_explicit(a->global_epoch,
                                            memory_order_relaxed);
        atomic_store_explicit(&en->lru_epoch, ge, memory_order_relaxed);
        ok++;
    }
    uint64_t t1 = now_ns();
    a->completed = ok;
    a->total_ns = t1 - t0;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <num_buckets> <num_threads> <ops_per_thread>\n", argv[0]);
        return 1;
    }
    uint64_t num_buckets = strtoull(argv[1], 0, 0);
    int T = atoi(argv[2]);
    uint64_t ops_per_thread = strtoull(argv[3], 0, 0);

    uint64_t bytes = num_buckets * sizeof(struct Bucket);
    fprintf(stderr, "[synth] buckets=%lu T=%d ops/thr=%lu bytes=%.2f MiB\n",
            num_buckets, T, ops_per_thread, bytes / (1024.0*1024.0));

    // Allocate cache_pool-sized region. Use anonymous + huge pages if possible.
    struct Bucket *buckets = (struct Bucket *)mmap(
        NULL, bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buckets == MAP_FAILED) { perror("mmap"); return 1; }
    // Try to get hugepages — gated by FUSEE_SYNTH_HUGEPAGE env (default on).
    // Set FUSEE_SYNTH_HUGEPAGE=0 to skip, mimicking FUSEE's mmap that does
    // NOT madvise hugepages on cache_pool.
    const char *hp = getenv("FUSEE_SYNTH_HUGEPAGE");
    if (!hp || atoi(hp) != 0) {
        madvise(buckets, bytes, MADV_HUGEPAGE);
    } else {
        fprintf(stderr, "[synth] hugepage advise SKIPPED (FUSEE_SYNTH_HUGEPAGE=0)\n");
    }

    // Touch to populate (random pattern of init keys)
    for (uint64_t i = 0; i < num_buckets; i++) {
        for (int e = 0; e < ENTRIES_PER_BUCKET; e++) {
            atomic_store(&buckets[i].entries[e].key, i * ENTRIES_PER_BUCKET + e);
            buckets[i].entries[e].value_size = ENTRY_VALUE_BYTES;
            // Touch first cacheline to commit allocation
            buckets[i].entries[e].value_bytes[0] = (uint8_t)i;
        }
    }
    _Atomic uint64_t global_epoch = 1;

    // Generate Zipf indices (s=0.99). Pool 2M entries — used as a sample
    // basket; each thread mod-indexes into it for a per-thread Zipf draw.
    uint64_t pool_size = 2 * 1024 * 1024;
    uint32_t *zipf_pool = (uint32_t*)malloc(sizeof(uint32_t) * pool_size);
    if (!zipf_pool) { perror("zipf"); return 1; }
    uint64_t key_range = num_buckets;  // operate over full bucket space
    if (key_range > 1000000) key_range = 1000000;  // cap CDF cost
    gen_zipf_indices(zipf_pool, pool_size, key_range, 0.99);

    pthread_t *th = malloc(sizeof(pthread_t) * T);
    struct ThreadArg *args = (struct ThreadArg*)calloc(T, sizeof(struct ThreadArg));
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 0; i < T; i++) {
        args[i].tid = i;
        args[i].cpu = i % ncpu;
        args[i].num_buckets = num_buckets;
        args[i].ops = ops_per_thread;
        args[i].buckets = buckets;
        args[i].global_epoch = &global_epoch;
        args[i].zipf_indices = zipf_pool;
        args[i].zipf_pool_size = pool_size;
    }
    uint64_t t0 = now_ns();
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, &args[i]);
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    uint64_t t1 = now_ns();

    uint64_t total_ops = 0; uint64_t total_ns_sum = 0;
    for (int i = 0; i < T; i++) {
        total_ops += args[i].completed;
        total_ns_sum += args[i].total_ns;
    }
    double wall_s = (t1 - t0) / 1e9;
    double aggr_mops = total_ops / wall_s / 1e6;
    double per_op_ns = (double)total_ns_sum / total_ops;
    printf("SYNTH buckets=%lu T=%d total_ops=%lu wall=%.3fs aggr_thpt=%.2f Mops per_op=%.1f ns ws_MiB=%.1f\n",
           num_buckets, T, total_ops, wall_s, aggr_mops, per_op_ns,
           bytes / (1024.0*1024.0));
    return 0;
}
