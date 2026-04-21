/*
 * abc_bench.c - Compare three CXL write protocols on YCSB workloads.
 *
 *   Option A: Sync Replication (writer waits for all ACKs before commit)
 *   Option B: Eager Push (writer commits, then pushes invalidations, no wait)
 *   Option C: Lazy RC     (writer just bumps epoch; readers pull on demand)
 *
 * Uses raw mmap on tmpfs (same as other benches). Multi-process mode.
 *
 * Usage (run once per simulated node, SAME tmpfs path):
 *   ./abc_bench --opt=A|B|C --workload=A|C --nodes=3 --node-id=0 \
 *               --threads=4 --ops=100000 [--path=/dev/shm/abc_bench.bin]
 *
 *   Launch all nodes roughly simultaneously (they don't wait for each other
 *   explicitly; just need to overlap). node-id=0 initializes the region.
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

#define NUM_BUCKETS       8192
#define MAX_THREADS       16
#define OP_STAGING_STORES 4       /* simulated KV+oplog+staging writes */
#define LAT_SAMPLE_CAP    200000  /* per-thread */

typedef enum { OPT_A, OPT_B, OPT_C } opt_t;
typedef enum { WL_A, WL_C } wl_t;  /* YCSB A = 50% write, C = 100% read */

struct BucketEntry {
    shm_mutex_t    lock;
    cacheline_u64  value;
    cacheline_u64  write_epoch;
    cacheline_u64  pending_op;
    cacheline_u64  ack[MAX_HOST_NUM];
    cacheline_u64  inval[MAX_HOST_NUM];
};

struct SharedRegion {
    cacheline_u64      magic;   /* 0 -> uninit, MAGIC -> initialized */
    cacheline_u64      start_barrier[MAX_HOST_NUM];
    cacheline_u64      workers_done[MAX_HOST_NUM];   /* node i's workers finished */
    struct BucketEntry buckets[NUM_BUCKETS];
};

#define MAGIC 0xABC123DEADBEEFULL

struct ThreadArg {
    int            thread_id;
    int            node_id;
    int            num_nodes;
    int            num_ops;
    double         write_ratio;
    opt_t          opt;
    struct SharedRegion *R;

    uint64_t total_ns;
    uint64_t num_reads;
    uint64_t num_writes;
    uint64_t *lat_w;
    uint64_t *lat_r;
    uint64_t  cnt_w;
    uint64_t  cnt_r;
};

static volatile atomic_int repl_stop = 0;

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* =========================================================== */
/*  Write variants                                             */
/* =========================================================== */

static void write_A(struct ThreadArg *a, int b) {
    struct BucketEntry *e = &a->R->buckets[b];
    uint64_t op_id = ((uint64_t)a->node_id << 48) |
                     ((uint64_t)a->thread_id << 40) |
                     (uint64_t)(now_ns() & 0xFFFFFFFFFFULL);

    shm_mutex_lock(&e->lock, a->node_id, a->num_nodes);

    (void)CACHELINE_LOAD(&e->value);

    for (int i = 0; i < OP_STAGING_STORES; i++) {
        CACHELINE_STORE(&e->value, op_id + i);
    }

    CACHELINE_STORE(&e->pending_op, op_id);

    int others = a->num_nodes - 1;
    while (1) {
        int got = 0;
        for (int i = 0; i < a->num_nodes; i++) {
            if (i == a->node_id) continue;
            if (CACHELINE_LOAD(&e->ack[i]) == op_id) got++;
        }
        if (got >= others) break;
        relax_cpu();
    }

    CACHELINE_STORE(&e->value, op_id);
    CACHELINE_STORE(&e->pending_op, 0);

    shm_mutex_unlock(&e->lock, a->node_id);
}

static void write_B(struct ThreadArg *a, int b) {
    struct BucketEntry *e = &a->R->buckets[b];
    uint64_t op_id = ((uint64_t)a->node_id << 48) |
                     ((uint64_t)a->thread_id << 40) |
                     (uint64_t)(now_ns() & 0xFFFFFFFFFFULL);

    shm_mutex_lock(&e->lock, a->node_id, a->num_nodes);

    (void)CACHELINE_LOAD(&e->value);

    for (int i = 0; i < OP_STAGING_STORES; i++) {
        CACHELINE_STORE(&e->value, op_id + i);
    }

    CACHELINE_STORE(&e->value, op_id);

    for (int i = 0; i < a->num_nodes; i++) {
        if (i != a->node_id) {
            CACHELINE_STORE(&e->inval[i], op_id);
        }
    }

    shm_mutex_unlock(&e->lock, a->node_id);
}

static void write_C(struct ThreadArg *a, int b) {
    struct BucketEntry *e = &a->R->buckets[b];
    uint64_t op_id = ((uint64_t)a->node_id << 48) |
                     ((uint64_t)a->thread_id << 40) |
                     (uint64_t)(now_ns() & 0xFFFFFFFFFFULL);

    shm_mutex_lock(&e->lock, a->node_id, a->num_nodes);

    (void)CACHELINE_LOAD(&e->value);

    for (int i = 0; i < OP_STAGING_STORES; i++) {
        CACHELINE_STORE(&e->value, op_id + i);
    }

    CACHELINE_STORE(&e->value, op_id);
    CACHELINE_STORE(&e->write_epoch, op_id);

    shm_mutex_unlock(&e->lock, a->node_id);
}

/* =========================================================== */
/*  Read                                                       */
/* =========================================================== */

static void do_read(struct ThreadArg *a, int b) {
    struct BucketEntry *e = &a->R->buckets[b];
    if (a->opt == OPT_A || a->opt == OPT_B) {
        volatile uint64_t tmp = e->value.value;  /* local cache hit */
        (void)tmp;
    } else {
        (void)CACHELINE_LOAD(&e->value);         /* lazy RC strict read */
    }
}

/* =========================================================== */
/*  Replicator (A/B only)                                      */
/* =========================================================== */

struct ReplArg {
    int   node_id;
    int   num_nodes;
    opt_t opt;
    struct SharedRegion *R;
};

static int all_workers_done(struct SharedRegion *R, int num_nodes) {
    for (int i = 0; i < num_nodes; i++) {
        if (CACHELINE_LOAD(&R->workers_done[i]) == 0) return 0;
    }
    return 1;
}

static void *replicator_thread(void *p) {
    struct ReplArg *r = p;
    uint64_t *last_seen = calloc(NUM_BUCKETS, sizeof(uint64_t));

    while (!atomic_load(&repl_stop)) {
        for (int b = 0; b < NUM_BUCKETS; b++) {
            struct BucketEntry *e = &r->R->buckets[b];
            if (r->opt == OPT_A) {
                uint64_t p_id = CACHELINE_LOAD(&e->pending_op);
                if (p_id != 0 && p_id != last_seen[b]) {
                    for (int i = 0; i < OP_STAGING_STORES; i++) {
                        (void)CACHELINE_LOAD(&e->value);
                    }
                    CACHELINE_STORE(&e->ack[r->node_id], p_id);
                    last_seen[b] = p_id;
                }
            } else if (r->opt == OPT_B) {
                uint64_t p_id = CACHELINE_LOAD(&e->inval[r->node_id]);
                if (p_id != 0 && p_id != last_seen[b]) {
                    for (int i = 0; i < OP_STAGING_STORES; i++) {
                        (void)CACHELINE_LOAD(&e->value);
                    }
                    last_seen[b] = p_id;
                }
            }
        }
    }
    free(last_seen);
    return NULL;
}

/* =========================================================== */
/*  Worker                                                     */
/* =========================================================== */

static void *worker_thread(void *p) {
    struct ThreadArg *a = p;
    unsigned int seed = (unsigned)(a->node_id * 10007 + a->thread_id * 31 + time(NULL));

    a->lat_w = calloc(LAT_SAMPLE_CAP, sizeof(uint64_t));
    a->lat_r = calloc(LAT_SAMPLE_CAP, sizeof(uint64_t));

    uint64_t t_start = now_ns();
    for (int i = 0; i < a->num_ops; i++) {
        int b = rand_r(&seed) % NUM_BUCKETS;
        double coin = (double)rand_r(&seed) / (double)RAND_MAX;

        uint64_t t0 = now_ns();
        if (coin < a->write_ratio) {
            switch (a->opt) {
                case OPT_A: write_A(a, b); break;
                case OPT_B: write_B(a, b); break;
                case OPT_C: write_C(a, b); break;
            }
            uint64_t dt = now_ns() - t0;
            a->num_writes++;
            if (a->cnt_w < LAT_SAMPLE_CAP) a->lat_w[a->cnt_w++] = dt;
        } else {
            do_read(a, b);
            uint64_t dt = now_ns() - t0;
            a->num_reads++;
            if (a->cnt_r < LAT_SAMPLE_CAP) a->lat_r[a->cnt_r++] = dt;
        }
    }
    a->total_ns = now_ns() - t_start;
    return NULL;
}

/* =========================================================== */
/*  Main                                                       */
/* =========================================================== */

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s --opt=A|B|C --workload=A|C --nodes=N --node-id=X "
        "--threads=T --ops=OPS [--path=PATH]\n", p);
    exit(1);
}

int main(int argc, char **argv) {
    opt_t opt = OPT_C;
    wl_t  wl  = WL_A;
    int   num_nodes = 3;
    int   node_id = 0;
    int   threads = 4;
    int   ops = 100000;
    const char *path = "/dev/shm/abc_bench.bin";

    static struct option longopts[] = {
        {"opt",      required_argument, 0, 'o'},
        {"workload", required_argument, 0, 'w'},
        {"nodes",    required_argument, 0, 'n'},
        {"node-id",  required_argument, 0, 'i'},
        {"threads",  required_argument, 0, 't'},
        {"ops",      required_argument, 0, 's'},
        {"path",     required_argument, 0, 'p'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "o:w:n:i:t:s:p:", longopts, NULL)) != -1) {
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
            case 'n': num_nodes = atoi(optarg); break;
            case 'i': node_id = atoi(optarg); break;
            case 't': threads = atoi(optarg); break;
            case 's': ops = atoi(optarg); break;
            case 'p': path = optarg; break;
            default:  usage(argv[0]);
        }
    }

    if (num_nodes > MAX_HOST_NUM || threads > MAX_THREADS) {
        fprintf(stderr, "too many nodes/threads\n"); return 1;
    }

    double write_ratio = (wl == WL_A) ? 0.5 : 0.0;
    /* devdax requires 2MB-aligned mmap size; round up. */
    size_t needed = sizeof(struct SharedRegion);
    size_t align = 2 * 1024 * 1024;
    size_t region_size = (needed + align - 1) & ~(align - 1);

    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { perror("open"); return 1; }
    /* ftruncate will fail on char devdax devices with EINVAL; ignore those */
    if (ftruncate(fd, region_size) < 0 && errno != EINVAL) {
        perror("ftruncate"); return 1;
    }

    struct SharedRegion *R = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
    if (R == MAP_FAILED) { perror("mmap"); return 1; }

    if (node_id == 0) {
        memset(R, 0, region_size);
        for (int b = 0; b < NUM_BUCKETS; b++) {
            shm_mutex_init(&R->buckets[b].lock);
        }
        CACHELINE_STORE(&R->magic, MAGIC);
        fprintf(stderr, "[node 0] initialized region (%.1f MB)\n",
                region_size / (1024.0*1024.0));
    } else {
        fprintf(stderr, "[node %d] waiting for init...\n", node_id);
        while (CACHELINE_LOAD(&R->magic) != MAGIC) {
            usleep(50000);
        }
        fprintf(stderr, "[node %d] attached\n", node_id);
    }

    /* Barrier: all nodes sync start */
    CACHELINE_STORE(&R->start_barrier[node_id], 1);
    for (int i = 0; i < num_nodes; i++) {
        while (CACHELINE_LOAD(&R->start_barrier[i]) == 0) usleep(10000);
    }
    fprintf(stderr, "[node %d] starting benchmark...\n", node_id);

    pthread_t repl_tid = 0;
    struct ReplArg repl_arg = {
        .node_id = node_id, .num_nodes = num_nodes, .opt = opt, .R = R
    };
    if (opt != OPT_C) {
        pthread_create(&repl_tid, NULL, replicator_thread, &repl_arg);
    }

    struct ThreadArg *args = calloc(threads, sizeof(*args));
    pthread_t *tids = calloc(threads, sizeof(*tids));
    for (int i = 0; i < threads; i++) {
        args[i] = (struct ThreadArg){
            .thread_id = i, .node_id = node_id,
            .num_nodes = num_nodes, .num_ops = ops / threads,
            .write_ratio = write_ratio, .opt = opt, .R = R
        };
        pthread_create(&tids[i], NULL, worker_thread, &args[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(tids[i], NULL);

    /* Signal "my workers done" so other nodes can progress */
    CACHELINE_STORE(&R->workers_done[node_id], 1);

    if (opt != OPT_C) {
        /* Keep replicator running until ALL nodes' workers finish */
        while (!all_workers_done(R, num_nodes)) {
            usleep(10000);
        }
        atomic_store(&repl_stop, 1);
        pthread_join(repl_tid, NULL);
    }

    /* Aggregate */
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

    printf("[node %d opt=%c wl=%c threads=%d] total=%lu ops in %.3fs\n",
           node_id, "ABC"[opt], "AC"[wl], threads,
           (unsigned long)(nr + nw), total_ns / 1e9);
    printf("  throughput: %.0f ops/sec\n", thpt);
    printf("  RESULT node_id=%d opt=%c wl=%c thpt=%.0f",
           node_id, "ABC"[opt], "AC"[wl], thpt);

    #define STAT(arr, cnt, tag) do {                                           \
        if (cnt > 0) {                                                         \
            qsort(arr, cnt, sizeof(uint64_t), cmp_u64);                        \
            uint64_t avg = 0;                                                  \
            for (uint64_t i = 0; i < cnt; i++) avg += arr[i];                  \
            avg /= cnt;                                                        \
            printf("  " tag "_avg=%.2f " tag "_p50=%.2f " tag "_p99=%.2f",     \
                   avg / 1000.0,                                               \
                   arr[cnt / 2] / 1000.0,                                      \
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
