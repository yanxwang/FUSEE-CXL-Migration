// FUSEE Figure 10 microbenchmark — port of FUSEE's micro-test/latency_test.cc
// to Protocol F.
//
// Reference: /home/yanwang/g1/FUSEE/micro-test/latency_test.cc
//   - WORKLOAD_NUM = 100,000 ops per type
//   - Single client, no warmup
//   - Sequential keys 0..N-1 (FUSEE's `load_seq_kv_requests`)
//   - Order: INSERT → SEARCH → UPDATE → DELETE (matches
//     latency_test_client.cc main())
//   - gettimeofday() µs precision per op
//   - Raw latency per op dumped to file
//
// Differences from FUSEE intentional:
//   - We use a u64 key (FUSEE uses string keys). Sequential u64 1..N
//     mirrors FUSEE's sequential key pattern.
//   - We use a u64 value (FUSEE uses string values).  Both are size-
//     equivalent for the protocol's slot/KV-pair handling.
//
// Usage:
//   cxl_kv_ops_F_microbench <dev_path> <host_id> <num_hosts> [N=100000]
// Output files: results/{insert,search,update,delete}_lat-Fp.txt
//   (Pp suffix lets us slot the dump beside FUSEE's existing files if any.)

#include "cxl_kv_ops_F.h"
#include "cxl_kv_ops_F_decomp.h"
#include "cxl_mm.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>

using namespace fusee;

namespace {

constexpr uint32_t kNumBuckets    = 32768;     // 32 K * 7 = 229 K slot caps
constexpr uint64_t kTotalRecords  = 4000000;   // 4× UPDATE churn headroom
constexpr int      kDefaultN      = 100000;    // FUSEE WORKLOAD_NUM
constexpr uint32_t kKvRecordSize  = 1024;      // paper §6.3 alignment
constexpr uint32_t kValueLen      = 1024 - 8;  // 1016 B value past 8 B key

uint64_t mk(uint64_t i) { return i + 1; }     // shift so key 0 (sentinel) is avoided

// Fill a 1016 B value buffer with a deterministic pattern keyed off `marker`
// so SEARCH can validate (if it ever needs to; current bench only times).
inline void fill_value_buf(uint8_t *buf, uint64_t marker) {
  uint64_t *p = reinterpret_cast<uint64_t *>(buf);
  for (uint32_t i = 0; i < kValueLen / 8; i++) {
    p[i] = marker + i;
  }
}

// Time a single op in microseconds, matching FUSEE's
// `(et.tv_sec - st.tv_sec) * 1000000 + (et.tv_usec - st.tv_usec)`.
// We additionally capture ns via steady_clock for sub-µs detail.

template <typename Fn>
uint64_t time_one_us(Fn op) {
  struct timeval st, et;
  gettimeofday(&st, nullptr);
  op();
  gettimeofday(&et, nullptr);
  return (et.tv_sec - st.tv_sec) * 1000000 + (et.tv_usec - st.tv_usec);
}

int dump_to_file(const char *fname, const uint64_t *lat, int n) {
  FILE *fp = std::fopen(fname, "w");
  if (!fp) { std::perror(fname); return -1; }
  for (int i = 0; i < n; i++) std::fprintf(fp, "%lu\n", lat[i]);
  std::fclose(fp);
  return 0;
}

// FUSEE latency_test.cc::test_lat — one op type, N ops, sequential keys.
// All ops use 1024 B KV (8 B key + 1016 B value) per paper §6.3.
int test_lat_insert(CxlKvStoreF &store, int N, uint64_t *lat) {
  int num_failed = 0;
  alignas(8) uint8_t vbuf[kValueLen];
  for (int i = 0; i < N; i++) {
    uint64_t k = mk(i);
    fill_value_buf(vbuf, static_cast<uint64_t>(i) * 31 + 9);
    lat[i] = time_one_us([&]() {
      int rc = store.insert_blob(k, vbuf, kValueLen);
      (void)rc;
    });
  }
  // Post-condition (not timed): every inserted key searchable.
  for (int i = 0; i < N; i++) {
    uint32_t want = kValueLen;
    if (store.search_blob(mk(i), vbuf, &want) != 0) num_failed++;
  }
  return num_failed;
}

int test_lat_search(CxlKvStoreF &store, int N, uint64_t *lat) {
  int num_failed = 0;
  alignas(8) uint8_t vbuf[kValueLen];
  for (int i = 0; i < N; i++) {
    uint64_t k = mk(i);
    int rc_local = 0;
    uint32_t want = kValueLen;
    lat[i] = time_one_us([&]() { rc_local = store.search_blob(k, vbuf, &want); });
    if (rc_local != 0) num_failed++;
  }
  return num_failed;
}

int test_lat_update(CxlKvStoreF &store, int N, uint64_t *lat) {
  int num_failed = 0;
  alignas(8) uint8_t vbuf[kValueLen];
  for (int i = 0; i < N; i++) {
    uint64_t k = mk(i);
    fill_value_buf(vbuf, 0xC0FFEEULL + i);
    int rc_local = 0;
    lat[i] = time_one_us([&]() { rc_local = store.update_blob(k, vbuf, kValueLen); });
    if (rc_local != 0) num_failed++;
  }
  return num_failed;
}

int test_lat_delete(CxlKvStoreF &store, int N, uint64_t *lat) {
  int num_failed = 0;
  for (int i = 0; i < N; i++) {
    uint64_t k = mk(i);
    int rc_local = 0;
    lat[i] = time_one_us([&]() { rc_local = store.remove(k); });
    if (rc_local != 0) num_failed++;
  }
  return num_failed;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr,
        "usage: %s <dev_path> <host_id> <num_hosts> [N=%d]\n",
        argv[0], kDefaultN);
    return 1;
  }
  const char *dev_path = argv[1];
  int host_id   = std::atoi(argv[2]);
  int num_hosts = std::atoi(argv[3]);
  int N = (argc >= 5) ? std::atoi(argv[4]) : kDefaultN;

  std::size_t store_bytes =
      CxlKvStoreF::bytes_for(kNumBuckets, kTotalRecords, num_hosts, kKvRecordSize);
  CXLRegion r{};
  if (cxl_region_init(&r, dev_path, store_bytes) != 0) {
    std::perror("cxl_region_init");
    return 1;
  }

  CxlKvStoreF store;
  int rc = store.attach(r.base, store_bytes, kNumBuckets, kTotalRecords,
                        host_id, num_hosts,
                        /*init_region=*/(host_id == 0),
                        kKvRecordSize);
  if (rc != 0) {
    std::fprintf(stderr, "host %d: attach failed rc=%d\n", host_id, rc);
    return 1;
  }
  std::fprintf(stderr, "host %d: attach OK; N=%d\n", host_id, N);

  // Non-operator host sits as a passive LFM peer so the lock's wait-arrays
  // see a real second host_id slot in use.  It exits when killed.
  if (host_id != 0) {
    while (true) std::this_thread::sleep_for(std::chrono::seconds(60));
  }

  mkdir("results", 0755);
  uint64_t *lat = static_cast<uint64_t *>(std::calloc(N, sizeof(uint64_t)));
  if (!lat) { std::perror("calloc"); return 1; }

  FILE *decomp_fp = std::fopen("results/decomp_stages-Fp.csv", "w");
  if (!decomp_fp) std::perror("decomp csv");

  auto dump_and_reset = [&](const char *op) {
    if (decomp_fp) {
      char prefix[32];
      std::snprintf(prefix, sizeof(prefix), "%s,", op);
      fusee::f_decomp_dump_stages(prefix, decomp_fp);
    }
    fusee::f_decomp_reset();
  };

  // INSERT
  std::fprintf(stderr, "lat test INSERT\n");
  fusee::f_decomp_reset();
  int nf = test_lat_insert(store, N, lat);
  std::fprintf(stderr, "INSERT failed=%d\n", nf);
  dump_to_file("results/insert_lat-Fp.txt", lat, N);
  dump_and_reset("insert");

  // SEARCH
  std::fprintf(stderr, "lat test SEARCH\n");
  nf = test_lat_search(store, N, lat);
  std::fprintf(stderr, "SEARCH failed=%d\n", nf);
  dump_to_file("results/search_lat-Fp.txt", lat, N);
  dump_and_reset("search");

  // UPDATE
  std::fprintf(stderr, "lat test UPDATE\n");
  nf = test_lat_update(store, N, lat);
  std::fprintf(stderr, "UPDATE failed=%d\n", nf);
  dump_to_file("results/update_lat-Fp.txt", lat, N);
  dump_and_reset("update");

  // DELETE
  std::fprintf(stderr, "lat test DELETE\n");
  nf = test_lat_delete(store, N, lat);
  std::fprintf(stderr, "DELETE failed=%d\n", nf);
  dump_to_file("results/delete_lat-Fp.txt", lat, N);
  dump_and_reset("delete");

  if (decomp_fp) std::fclose(decomp_fp);
  std::free(lat);
  store.stop();
  cxl_region_destroy(&r);
  return 0;
}
