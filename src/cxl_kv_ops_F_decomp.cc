#include "cxl_kv_ops_F_decomp.h"

#include <algorithm>
#include <cstdio>

namespace fusee {

namespace {
const char *kStageNames[kF_DECOMP_COUNT] = {
    "INS_pre_flush",  "INS_pre_scan",   "INS_alloc",      "INS_write_pair",
    "INS_lock",       "INS_verify",     "INS_publish",    "INS_unlock",
    "INS_cache",      "INS_TOTAL",
    "UPD_flush_scan", "UPD_alloc_write","UPD_lock",       "UPD_verify",
    "UPD_publish",    "UPD_unlock",     "UPD_free",       "UPD_cache",
    "UPD_TOTAL",
    "DEL_flush_scan", "DEL_lock",       "DEL_verify",     "DEL_clear",
    "DEL_unlock",     "DEL_free",       "DEL_cache",      "DEL_TOTAL",
    "SRC_cache_lookup","SRC_fast_path", "SRC_slow_path",  "SRC_cache_update",
    "SRC_TOTAL",
};
}  // namespace

const char *f_decomp_name(int stage) {
  if (stage < 0 || stage >= kF_DECOMP_COUNT) return "?";
  return kStageNames[stage];
}

FDecompProbe &f_decomp_probe() {
  static thread_local FDecompProbe inst;
  return inst;
}

void f_decomp_reset() {
  auto &p = f_decomp_probe();
  for (int s = 0; s < kF_DECOMP_COUNT; s++) p.stage_ns[s].clear();
}

static uint32_t pct(std::vector<uint32_t> &v, double p) {
  if (v.empty()) return 0;
  size_t idx = static_cast<size_t>(p * (v.size() - 1));
  return v[idx];
}

void f_decomp_dump_stages(const char *prefix, FILE *out) {
  if (!out) out = stdout;
  std::fprintf(out,
      "%sstage,count,avg_ns,p50_ns,p90_ns,p99_ns,p999_ns,min_ns,max_ns\n",
      prefix ? prefix : "");
  auto &p = f_decomp_probe();
  for (int s = 0; s < kF_DECOMP_COUNT; s++) {
    auto &v = p.stage_ns[s];
    if (v.empty()) continue;
    std::sort(v.begin(), v.end());
    uint64_t sum = 0;
    for (auto x : v) sum += x;
    std::fprintf(out, "%s%s,%zu,%lu,%u,%u,%u,%u,%u,%u\n",
                 prefix ? prefix : "",
                 f_decomp_name(s),
                 v.size(), sum / v.size(),
                 pct(v, 0.50), pct(v, 0.90), pct(v, 0.99),
                 pct(v, 0.999),
                 v.front(), v.back());
  }
  std::fflush(out);
}

}  // namespace fusee
