/*
 * ycsb_abc_bench.c  —  FUSEE-style CXL KV store mini with A/B/C protocols.
 *
 * Simulates a small FUSEE-like KV store on CXL shared memory:
 *  - RACE-hash-style: buckets of 7 slots, fingerprint-based quick match
 *  - LFM per-bucket locking
 *  - KV data stored in a per-node local region (with CXL staging buffer for repl)
 *  - 3 write protocols: A (sync replication), B (eager push), C (lazy RC)
 *
 * Run: same cmdline as abc_bench; YCSB-style key/value strings are used.
 *
 *   ./ycsb_abc_bench --opt=A|B|C --workload=A|C --nodes=N --node-id=X \
 *                    --threads=T --ops=OPS [--path=/dev/dax0.0]
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdatomic.h>

#include "../shm_mutex.h"
#include "../common.h"

/* ================================================================
 *  Tunables
 * ================================================================ */
#define NUM_BUCKETS       (1 << 12)   /* 4096 buckets */
#define SLOTS_PER_BUCKET  7
#define MAX_KEY_LEN       24
#define MAX_VAL_LEN       100
#define OP_STAGING_STORES 4           /* sim cost of staging writes */
#define LAT_SAMPLE_CAP    200000
#define MAX_THREADS       16
#define KV_SLAB_BYTES     (128 * 1024 * 1024)  /* 128 MB per-node KV */

/* ================================================================
 *  Types
 * ================================================================ */
typedef enum { OPT_A, OPT_B, OPT_C } opt_t;
typedef enum { WL_A, WL_C, WL_CUSTOM } wl_t;

/* 8-byte slot: fp(1) + kv_len(1) + node_id(1) + ptr(5) */
typedef struct __attribute__((packed)) {
    uint8_t  fp;
    uint8_t  kv_len;
    uint8_t  node_id;
    uint8_t  ptr[5];          /* 40-bit offset into owner's KV area */
} Slot;

typedef struct {
    Slot slots[SLOTS_PER_BUCKET];
    uint8_t pad[8];  /* pad to 64B */
} HashBucket;                    /* exactly 64B */

/* Per-bucket lock entry (cache-line aligned).
 * For option A: pending/ack live here. For C: write_epoch lives here.
 * For B: invalidation list in a separate per-node region. */
typedef struct {
    shm_mutex_t   lock;                                /* LFM */
    HashBucket    bucket;                              /* 64B authoritative data */
    cacheline_u64 write_epoch;                         /* C: bumped on commit */
    cacheline_u64 pending_op;                          /* A: op_id or 0 */
    cacheline_u64 pending_slot;                        /* A: which slot */
    cacheline_u64 pending_new_value;                   /* A: packed Slot */
    cacheline_u64 ack[MAX_HOST_NUM];                   /* A: per-node ack */
} BucketLockEntry;

/* Per-node invalidation list for Option B */
#define INVAL_RING_ENTRIES  4096
typedef struct {
    cacheline_u64 head;            /* consumer */
    cacheline_u64 tail;            /* producer */
    struct {
        uint32_t bucket_idx;
        uint32_t slot_idx;
        uint64_t new_value;
    } entries[INVAL_RING_ENTRIES];
} InvalRing;

/* Per-node KV region in CXL (holds key+value blobs keyed by offset).
 * Simplified: used as a flat bump allocator, no free for this bench. */
typedef struct {
    cacheline_u64 head;                 /* bump allocator cursor */
    uint8_t data[KV_SLAB_BYTES - 64];   /* kv bytes */
} NodeKVArea;

typedef struct {
    cacheline_u64    magic;
    cacheline_u64    start_barrier[MAX_HOST_NUM];
    cacheline_u64    workers_done[MAX_HOST_NUM];
    BucketLockEntry  buckets[NUM_BUCKETS];
    InvalRing        inval[MAX_HOST_NUM];
    NodeKVArea       kv[MAX_HOST_NUM];
} SharedRegion;

#define MAGIC 0xCFF55E0001A0B0CULL

/* ================================================================
 *  Helpers
 * ================================================================ */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* CRC-like 64b hash (FNV-1a) */
static uint64_t fnv1a(const uint8_t *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint8_t compute_fp(uint64_t hash) {
    uint8_t fp = 0;
    fp ^= (uint8_t)(hash >> 48);
    fp ^= (uint8_t)(hash >> 56);
    return fp == 0 ? 1 : fp;   /* avoid 0 (empty marker) */
}

static uint64_t slot_to_u64(const Slot *s) {
    uint64_t v = 0;
    memcpy(&v, s, 8);
    return v;
}
static Slot u64_to_slot(uint64_t v) {
    Slot s;
    memcpy(&s, &v, 8);
    return s;
}
static uint64_t slot_ptr_to_u64(const Slot *s) {
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v |= ((uint64_t)s->ptr[i]) << (8 * i);
    return v;
}
static void slot_ptr_from_u64(uint64_t off, Slot *s) {
    for (int i = 0; i < 5; i++) s->ptr[i] = (off >> (8 * i)) & 0xFF;
}

/* ================================================================
 *  Global state (per process)
 * ================================================================ */
static SharedRegion *R;
static int           g_node_id;
static int           g_num_nodes;
static opt_t         g_opt;
static volatile atomic_int repl_stop = 0;

/* Local KV cursor (per-node bump allocator; atomic so multiple threads are safe) */
static atomic_uint_fast64_t local_kv_cursor = 0;   /* offset into R->kv[my_node_id].data */

/* ================================================================
 *  KV allocation (bump allocator in CXL node area; atomic fetch-add)
 * ================================================================ */
static uint64_t alloc_kv(uint32_t size) {
    uint64_t off = atomic_fetch_add(&local_kv_cursor, size);
    if (off + size >= sizeof(R->kv[0].data)) return UINT64_MAX;
    return off;
}

/* Write KV blob: format = [u16 key_len][u16 val_len][key][value], cache-line flushed.
 * returns offset into node's kv area, or UINT64_MAX on fail. */
static uint64_t store_kv_local(int node, const uint8_t *key, uint16_t klen,
                               const uint8_t *val, uint16_t vlen) {
    uint32_t total = 4 + klen + vlen;
    uint64_t off = alloc_kv(total);
    if (off == UINT64_MAX) return off;
    uint8_t *dst = R->kv[node].data + off;
    memcpy(dst, &klen, 2);
    memcpy(dst+2, &vlen, 2);
    memcpy(dst+4, key, klen);
    memcpy(dst+4+klen, val, vlen);
    /* flush entire KV block (multiple cache lines) */
    for (size_t i = 0; i < total; i += CACHE_LINE_SIZE) {
        flush_line(dst + i);
    }
    store_fence();
    return off;
}

static int cmp_kv_key(int owner_node, uint64_t off,
                      const uint8_t *key, uint16_t klen) {
    uint8_t *src = R->kv[owner_node].data + off;
    flush_line(src);
    full_fence();
    uint16_t slen;
    memcpy(&slen, src, 2);
    if (slen != klen) return 1;
    /* flush more lines if key spans */
    for (size_t i = CACHE_LINE_SIZE; i < (size_t)(4 + klen); i += CACHE_LINE_SIZE) {
        flush_line(src + i);
    }
    full_fence();
    return memcmp(src + 4, key, klen);
}

static void fetch_kv_value(int owner_node, uint64_t off,
                           uint8_t *val_out, uint16_t *vlen_out) {
    uint8_t *src = R->kv[owner_node].data + off;
    uint16_t klen, vlen;
    flush_line(src);
    full_fence();
    memcpy(&klen, src, 2);
    memcpy(&vlen, src+2, 2);
    uint8_t *val_src = src + 4 + klen;
    for (size_t i = 0; i < (size_t)(4 + klen + vlen); i += CACHE_LINE_SIZE) {
        flush_line(src + i);
    }
    full_fence();
    memcpy(val_out, val_src, vlen);
    *vlen_out = vlen;
}

/* ================================================================
 *  Core: bucket scan + slot constructed
 * ================================================================ */
static int scan_bucket_for_empty(const HashBucket *b) {
    for (int i = 0; i < SLOTS_PER_BUCKET; i++) {
        if (b->slots[i].fp == 0) return i;
    }
    return -1;
}

static int scan_bucket_match_fp(const HashBucket *b, uint8_t fp,
                                int *found_slot_indices) {
    int n = 0;
    for (int i = 0; i < SLOTS_PER_BUCKET; i++) {
        if (b->slots[i].fp == fp) {
            found_slot_indices[n++] = i;
        }
    }
    return n;
}

static void build_slot(Slot *s, uint8_t fp, uint8_t kv_len,
                       int node_id, uint64_t ptr_off) {
    s->fp = fp;
    s->kv_len = kv_len;
    s->node_id = (uint8_t)node_id;
    slot_ptr_from_u64(ptr_off, s);
}

/* ================================================================
 *  Option A: sync replication write
 * ================================================================ */
static int kv_insert_A(const uint8_t *key, uint16_t klen,
                      const uint8_t *val, uint16_t vlen) {
    uint64_t hash = fnv1a(key, klen);
    uint8_t  fp   = compute_fp(hash);
    int      b    = hash % NUM_BUCKETS;
    BucketLockEntry *e = &R->buckets[b];

    /* 1. Allocate + stage KV in our own CXL NodeKVArea */
    uint64_t off = store_kv_local(g_node_id, key, klen, val, vlen);
    if (off == UINT64_MAX) return -1;

    shm_mutex_lock(&e->lock, g_node_id, g_num_nodes);

    /* 2. Load authoritative bucket, find empty slot */
    HashBucket tmp;
    for (int i = 0; i < SLOTS_PER_BUCKET; i++) {
        uint64_t sv = CACHELINE_LOAD((cacheline_u64*)&e->bucket.slots[i]);
        (void)sv;  /* force flush */
        tmp.slots[i] = e->bucket.slots[i];
    }
    int empty_idx = scan_bucket_for_empty(&tmp);
    if (empty_idx < 0) {
        shm_mutex_unlock(&e->lock, g_node_id);
        return -2;  /* bucket full */
    }

    Slot new_slot;
    build_slot(&new_slot, fp, 1, g_node_id, off);
    uint64_t new_val = slot_to_u64(&new_slot);

    /* 3. Simulated staging writes (to mimic OpLog + staging protocol cost) */
    for (int i = 0; i < OP_STAGING_STORES; i++) {
        CACHELINE_STORE(&e->pending_new_value, new_val + i);
    }

    /* 4. Set pending op, ack[self]=op_id */
    uint64_t op_id = ((uint64_t)g_node_id << 48) | (now_ns() & 0xFFFFFFFFFFULL);
    CACHELINE_STORE(&e->pending_op, op_id);
    CACHELINE_STORE(&e->pending_slot, empty_idx);
    CACHELINE_STORE(&e->pending_new_value, new_val);

    /* 5. Spin-wait for all other nodes' ack */
    int others = g_num_nodes - 1;
    while (1) {
        int got = 0;
        for (int i = 0; i < g_num_nodes; i++) {
            if (i == g_node_id) continue;
            if (CACHELINE_LOAD(&e->ack[i]) == op_id) got++;
        }
        if (got >= others) break;
        relax_cpu();
    }

    /* 6. Commit */
    uint64_t *slot_u = (uint64_t *)&e->bucket.slots[empty_idx];
    CACHELINE_STORE((cacheline_u64*)slot_u, new_val);
    CACHELINE_STORE(&e->pending_op, 0);

    shm_mutex_unlock(&e->lock, g_node_id);
    return 0;
}

/* ================================================================
 *  Option B: eager push
 * ================================================================ */
static int kv_insert_B(const uint8_t *key, uint16_t klen,
                      const uint8_t *val, uint16_t vlen) {
    uint64_t hash = fnv1a(key, klen);
    uint8_t  fp   = compute_fp(hash);
    int      b    = hash % NUM_BUCKETS;
    BucketLockEntry *e = &R->buckets[b];

    uint64_t off = store_kv_local(g_node_id, key, klen, val, vlen);
    if (off == UINT64_MAX) return -1;

    shm_mutex_lock(&e->lock, g_node_id, g_num_nodes);

    HashBucket tmp;
    for (int i = 0; i < SLOTS_PER_BUCKET; i++) {
        (void)CACHELINE_LOAD((cacheline_u64*)&e->bucket.slots[i]);
        tmp.slots[i] = e->bucket.slots[i];
    }
    int empty_idx = scan_bucket_for_empty(&tmp);
    if (empty_idx < 0) {
        shm_mutex_unlock(&e->lock, g_node_id);
        return -2;
    }

    Slot new_slot;
    build_slot(&new_slot, fp, 1, g_node_id, off);
    uint64_t new_val = slot_to_u64(&new_slot);

    for (int i = 0; i < OP_STAGING_STORES; i++) {
        CACHELINE_STORE(&e->pending_new_value, new_val + i);
    }

    /* commit directly */
    uint64_t *slot_u = (uint64_t *)&e->bucket.slots[empty_idx];
    CACHELINE_STORE((cacheline_u64*)slot_u, new_val);

    /* push invalidation */
    for (int i = 0; i < g_num_nodes; i++) {
        if (i == g_node_id) continue;
        uint64_t t = CACHELINE_LOAD(&R->inval[i].tail);
        uint64_t idx = t % INVAL_RING_ENTRIES;
        R->inval[i].entries[idx].bucket_idx = b;
        R->inval[i].entries[idx].slot_idx = empty_idx;
        R->inval[i].entries[idx].new_value = new_val;
        flush_line(&R->inval[i].entries[idx]);
        store_fence();
        CACHELINE_STORE(&R->inval[i].tail, t + 1);
    }

    shm_mutex_unlock(&e->lock, g_node_id);
    return 0;
}

/* ================================================================
 *  Option C: lazy RC
 * ================================================================ */
static int kv_insert_C(const uint8_t *key, uint16_t klen,
                      const uint8_t *val, uint16_t vlen) {
    uint64_t hash = fnv1a(key, klen);
    uint8_t  fp   = compute_fp(hash);
    int      b    = hash % NUM_BUCKETS;
    BucketLockEntry *e = &R->buckets[b];

    uint64_t off = store_kv_local(g_node_id, key, klen, val, vlen);
    if (off == UINT64_MAX) return -1;

    shm_mutex_lock(&e->lock, g_node_id, g_num_nodes);

    HashBucket tmp;
    for (int i = 0; i < SLOTS_PER_BUCKET; i++) {
        (void)CACHELINE_LOAD((cacheline_u64*)&e->bucket.slots[i]);
        tmp.slots[i] = e->bucket.slots[i];
    }
    int empty_idx = scan_bucket_for_empty(&tmp);
    if (empty_idx < 0) {
        shm_mutex_unlock(&e->lock, g_node_id);
        return -2;
    }

    Slot new_slot;
    build_slot(&new_slot, fp, 1, g_node_id, off);
    uint64_t new_val = slot_to_u64(&new_slot);

    for (int i = 0; i < OP_STAGING_STORES; i++) {
        CACHELINE_STORE(&e->pending_new_value, new_val + i);
    }

    /* commit + bump epoch */
    uint64_t *slot_u = (uint64_t *)&e->bucket.slots[empty_idx];
    CACHELINE_STORE((cacheline_u64*)slot_u, new_val);
    uint64_t cur_ep = CACHELINE_LOAD(&e->write_epoch);
    CACHELINE_STORE(&e->write_epoch, cur_ep + 1);

    shm_mutex_unlock(&e->lock, g_node_id);
    return 0;
}

static int kv_insert(const uint8_t *key, uint16_t klen,
                     const uint8_t *val, uint16_t vlen) {
    switch (g_opt) {
        case OPT_A: return kv_insert_A(key, klen, val, vlen);
        case OPT_B: return kv_insert_B(key, klen, val, vlen);
        case OPT_C: return kv_insert_C(key, klen, val, vlen);
    }
    return -1;
}

/* ================================================================
 *  SEARCH
 * ================================================================ */
static int kv_search(const uint8_t *key, uint16_t klen,
                     uint8_t *val_out, uint16_t *vlen_out) {
    uint64_t hash = fnv1a(key, klen);
    uint8_t  fp   = compute_fp(hash);
    int      b    = hash % NUM_BUCKETS;
    BucketLockEntry *e = &R->buckets[b];

    /* For A/B: read local bucket directly. For C: strict read via CXL. */
    HashBucket tmp;
    if (g_opt == OPT_C) {
        for (int i = 0; i < SLOTS_PER_BUCKET; i++) {
            uint64_t v = CACHELINE_LOAD((cacheline_u64*)&e->bucket.slots[i]);
            tmp.slots[i] = u64_to_slot(v);
        }
    } else {
        /* plain read - simulates local cache hit */
        memcpy(&tmp, &e->bucket, sizeof(HashBucket));
    }

    int matches[SLOTS_PER_BUCKET];
    int nm = scan_bucket_match_fp(&tmp, fp, matches);
    for (int i = 0; i < nm; i++) {
        Slot *s = &tmp.slots[matches[i]];
        uint64_t off = slot_ptr_to_u64(s);
        if (cmp_kv_key(s->node_id, off, key, klen) == 0) {
            fetch_kv_value(s->node_id, off, val_out, vlen_out);
            return 0;
        }
    }
    return -1;  /* not found */
}

/* ================================================================
 *  Replicator (consumer of A.pending / B.inval)
 * ================================================================ */
struct ReplArg {
    int node_id;
    int num_nodes;
    opt_t opt;
};

static void *replicator_thread(void *p) {
    struct ReplArg *r = p;
    static uint64_t last_seen[NUM_BUCKETS] = {0};
    uint64_t last_head = 0;

    while (!atomic_load(&repl_stop)) {
        if (r->opt == OPT_A) {
            for (int b = 0; b < NUM_BUCKETS; b++) {
                BucketLockEntry *e = &R->buckets[b];
                uint64_t p_id = CACHELINE_LOAD(&e->pending_op);
                if (p_id != 0 && p_id != last_seen[b]) {
                    /* Simulated pull (4 CXL loads) */
                    for (int i = 0; i < OP_STAGING_STORES; i++) {
                        (void)CACHELINE_LOAD(&e->pending_new_value);
                    }
                    CACHELINE_STORE(&e->ack[r->node_id], p_id);
                    last_seen[b] = p_id;
                }
            }
        } else if (r->opt == OPT_B) {
            uint64_t tail = CACHELINE_LOAD(&R->inval[r->node_id].tail);
            while (last_head < tail) {
                uint64_t idx = last_head % INVAL_RING_ENTRIES;
                uint64_t v = R->inval[r->node_id].entries[idx].new_value;
                (void)v;
                /* simulated pull */
                for (int i = 0; i < OP_STAGING_STORES; i++) {
                    flush_line(&R->inval[r->node_id].entries[idx]);
                    full_fence();
                    (void)R->inval[r->node_id].entries[idx].new_value;
                }
                last_head++;
            }
            CACHELINE_STORE(&R->inval[r->node_id].head, last_head);
        }
    }
    return NULL;
}

/* ================================================================
 *  Worker thread (YCSB-like)
 * ================================================================ */
struct ThreadArg {
    int thread_id;
    int num_ops;
    double write_ratio;

    uint64_t total_ns;
    uint64_t num_reads;
    uint64_t num_writes;
    uint64_t *lat_w, *lat_r;
    uint64_t cnt_w, cnt_r;
};

static void make_key(uint8_t *buf, uint16_t *len, unsigned int *seed) {
    uint32_t v = rand_r(seed);
    int n = 8 + (rand_r(seed) % 8);
    for (int i = 0; i < n; i++) buf[i] = 'a' + ((v >> i) & 0x1F) % 26;
    *len = n;
}

static void make_val(uint8_t *buf, uint16_t *len, unsigned int *seed) {
    int n = 20 + (rand_r(seed) % 40);
    for (int i = 0; i < n; i++) buf[i] = 'A' + (rand_r(seed) & 0x1F);
    *len = n;
}

static void *worker_thread(void *p) {
    struct ThreadArg *a = p;
    unsigned int seed = (unsigned)(g_node_id * 1009 + a->thread_id * 17 + time(NULL));

    a->lat_w = calloc(LAT_SAMPLE_CAP, sizeof(uint64_t));
    a->lat_r = calloc(LAT_SAMPLE_CAP, sizeof(uint64_t));

    /* Preload: each thread INSERTs a few first so SEARCH has hits */
    int preload = 200;
    for (int i = 0; i < preload; i++) {
        uint8_t k[32], v[128];
        uint16_t kl, vl;
        make_key(k, &kl, &seed);
        make_val(v, &vl, &seed);
        (void)kv_insert(k, kl, v, vl);
    }

    /* store preload keys for SEARCH retrieval */
    uint8_t *preload_keys = malloc(preload * MAX_KEY_LEN);
    uint16_t *preload_kls = calloc(preload, sizeof(uint16_t));
    unsigned int pseed = seed - preload * 17;  /* NOT the seed used above */
    for (int i = 0; i < preload; i++) {
        make_key(preload_keys + i * MAX_KEY_LEN, &preload_kls[i], &pseed);
    }

    uint64_t t_start = now_ns();
    for (int i = 0; i < a->num_ops; i++) {
        double coin = (double)rand_r(&seed) / RAND_MAX;
        uint64_t t0 = now_ns();
        if (coin < a->write_ratio) {
            uint8_t k[32], v[128];
            uint16_t kl, vl;
            make_key(k, &kl, &seed);
            make_val(v, &vl, &seed);
            kv_insert(k, kl, v, vl);
            uint64_t dt = now_ns() - t0;
            a->num_writes++;
            if (a->cnt_w < LAT_SAMPLE_CAP) a->lat_w[a->cnt_w++] = dt;
        } else {
            int idx = rand_r(&seed) % preload;
            uint8_t val[128];
            uint16_t vl;
            kv_search(preload_keys + idx * MAX_KEY_LEN, preload_kls[idx], val, &vl);
            uint64_t dt = now_ns() - t0;
            a->num_reads++;
            if (a->cnt_r < LAT_SAMPLE_CAP) a->lat_r[a->cnt_r++] = dt;
        }
    }
    a->total_ns = now_ns() - t_start;
    free(preload_keys); free(preload_kls);
    return NULL;
}

/* ================================================================
 *  Main
 * ================================================================ */
static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s --opt=A|B|C --workload=A|C --nodes=N --node-id=X "
        "--threads=T --ops=OPS [--path=/dev/dax0.0]\n", p);
    exit(1);
}

int main(int argc, char **argv) {
    opt_t opt = OPT_C;
    wl_t wl = WL_A;
    int num_nodes = 4;
    int node_id = 0;
    int threads = 2;
    int ops = 5000;
    const char *path = "/dev/dax0.0";

    double write_ratio_custom = -1.0;

    static struct option longopts[] = {
        {"opt",         required_argument, 0, 'o'},
        {"workload",    required_argument, 0, 'w'},
        {"write-ratio", required_argument, 0, 'r'},
        {"nodes",       required_argument, 0, 'n'},
        {"node-id",     required_argument, 0, 'i'},
        {"threads",     required_argument, 0, 't'},
        {"ops",         required_argument, 0, 's'},
        {"path",        required_argument, 0, 'p'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "o:w:r:n:i:t:s:p:", longopts, NULL)) != -1) {
        switch (c) {
            case 'o':
                if (optarg[0]=='A') opt=OPT_A;
                else if (optarg[0]=='B') opt=OPT_B;
                else if (optarg[0]=='C') opt=OPT_C;
                else usage(argv[0]);
                break;
            case 'w':
                if (optarg[0]=='A') wl=WL_A;
                else if (optarg[0]=='C') wl=WL_C;
                else usage(argv[0]);
                break;
            case 'r':
                wl = WL_CUSTOM;
                write_ratio_custom = atof(optarg);
                break;
            case 'n': num_nodes = atoi(optarg); break;
            case 'i': node_id = atoi(optarg); break;
            case 't': threads = atoi(optarg); break;
            case 's': ops = atoi(optarg); break;
            case 'p': path = optarg; break;
            default:  usage(argv[0]);
        }
    }

    g_opt = opt;
    g_node_id = node_id;
    g_num_nodes = num_nodes;
    if (num_nodes > MAX_HOST_NUM) { fprintf(stderr, "too many nodes\n"); return 1; }

    double write_ratio;
    if (wl == WL_CUSTOM) write_ratio = write_ratio_custom;
    else if (wl == WL_A) write_ratio = 0.5;
    else write_ratio = 0.0;

    /* devdax requires 2MB-aligned mmap size; round up. */
    size_t needed = sizeof(SharedRegion);
    size_t align = 2 * 1024 * 1024;
    size_t region_size_aligned = (needed + align - 1) & ~(align - 1);
    size_t region_size = region_size_aligned;

    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, region_size) < 0 && errno != EINVAL) {
        perror("ftruncate"); return 1;
    }

    R = mmap(NULL, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (R == MAP_FAILED) { perror("mmap"); return 1; }

    if (node_id == 0) {
        memset(R, 0, region_size);
        for (int b = 0; b < NUM_BUCKETS; b++)
            shm_mutex_init(&R->buckets[b].lock);
        CACHELINE_STORE(&R->magic, MAGIC);
        fprintf(stderr, "[node 0] init region (%.1f MB)\n",
                region_size / (1024.0*1024.0));
    } else {
        while (CACHELINE_LOAD(&R->magic) != MAGIC) usleep(50000);
        fprintf(stderr, "[node %d] attached\n", node_id);
    }

    CACHELINE_STORE(&R->start_barrier[node_id], 1);
    for (int i = 0; i < num_nodes; i++) {
        while (CACHELINE_LOAD(&R->start_barrier[i]) == 0) usleep(10000);
    }

    pthread_t repl_tid = 0;
    struct ReplArg repl_arg = {.node_id = node_id, .num_nodes = num_nodes, .opt = opt};
    if (opt != OPT_C) {
        pthread_create(&repl_tid, NULL, replicator_thread, &repl_arg);
    }

    struct ThreadArg *args = calloc(threads, sizeof(*args));
    pthread_t *tids = calloc(threads, sizeof(*tids));
    for (int i = 0; i < threads; i++) {
        args[i] = (struct ThreadArg){
            .thread_id = i, .num_ops = ops / threads,
            .write_ratio = write_ratio
        };
        pthread_create(&tids[i], NULL, worker_thread, &args[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(tids[i], NULL);

    CACHELINE_STORE(&R->workers_done[node_id], 1);
    if (opt != OPT_C) {
        while (1) {
            int all = 1;
            for (int i = 0; i < num_nodes; i++) {
                if (CACHELINE_LOAD(&R->workers_done[i]) == 0) { all = 0; break; }
            }
            if (all) break;
            usleep(10000);
        }
        atomic_store(&repl_stop, 1);
        pthread_join(repl_tid, NULL);
    }

    uint64_t total_ns = 0, nr = 0, nw = 0;
    uint64_t *all_w = calloc((size_t)threads * LAT_SAMPLE_CAP, sizeof(uint64_t));
    uint64_t *all_r = calloc((size_t)threads * LAT_SAMPLE_CAP, sizeof(uint64_t));
    uint64_t tw = 0, tr = 0;
    for (int i = 0; i < threads; i++) {
        if (args[i].total_ns > total_ns) total_ns = args[i].total_ns;
        nr += args[i].num_reads;
        nw += args[i].num_writes;
        memcpy(all_w + tw, args[i].lat_w, args[i].cnt_w * sizeof(uint64_t));
        tw += args[i].cnt_w;
        memcpy(all_r + tr, args[i].lat_r, args[i].cnt_r * sizeof(uint64_t));
        tr += args[i].cnt_r;
        free(args[i].lat_w); free(args[i].lat_r);
    }
    double thpt = (double)(nr + nw) * 1e9 / (double)total_ns;
    printf("[node %d opt=%c wratio=%.2f threads=%d] total=%lu ops in %.3fs\n",
           node_id, "ABC"[opt], write_ratio, threads,
           (unsigned long)(nr + nw), total_ns / 1e9);
    printf("  throughput: %.0f ops/sec\n", thpt);
    char wl_label = (wl == WL_A) ? 'A' : (wl == WL_C) ? 'C' : 'X';
    printf("  RESULT node_id=%d opt=%c wl=%c wratio=%.2f thpt=%.0f",
           node_id, "ABC"[opt], wl_label, write_ratio, thpt);

    #define STAT(arr, cnt, tag) do {                                           \
        if (cnt > 0) {                                                         \
            qsort(arr, cnt, sizeof(uint64_t), cmp_u64);                        \
            uint64_t avg = 0;                                                  \
            for (uint64_t i = 0; i < cnt; i++) avg += arr[i];                  \
            avg /= cnt;                                                        \
            printf("  " tag "_avg=%.2f " tag "_p50=%.2f " tag "_p99=%.2f",     \
                   avg / 1000.0, arr[cnt / 2] / 1000.0,                        \
                   arr[(cnt * 99) / 100] / 1000.0);                            \
        }                                                                      \
    } while (0)
    STAT(all_w, tw, "w");
    STAT(all_r, tr, "r");
    printf("\n");
    fflush(stdout);

    free(all_w); free(all_r); free(args); free(tids);
    munmap(R, region_size);
    close(fd);
    return 0;
}
