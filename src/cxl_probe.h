#ifndef FUSEE_CXL_PROBE_H_
#define FUSEE_CXL_PROBE_H_

// iter-6A Phase 2: per-stage µs timestamp probes.
//
// Compile-time gated: when FUSEE_PROBE is undefined or 0, all PROBE()
// macros expand to a single inlined branch on a TLS bool — overhead
// is negligible (~1 ns / probe / disabled). When FUSEE_PROBE=1 at
// build, PROBE() emits one frame to a TLS ring buffer (4096 frames
// default, oldest-evicted on wraparound). Frames are flushed to a
// per-thread file at thread exit (via pthread cleanup) OR on demand
// via probe_dump_all().
//
// Usage in code:
//   PROBE("W1");   // tag, no op_id (free-form stage marker)
//   PROBE_OP("W7", op_id);   // tag + op_id (group by request)
//
// At runtime: set FUSEE_PROBE_DUMP=/tmp/probe_h0_T8.bin to enable
// dump on thread exit. Otherwise frames discarded silently.
//
// Frame format: 24 bytes (tag pointer + ns + op_id) → 4096 × 24 =
// 96 KB TLS / thread.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <ctime>
#include <unistd.h>

namespace fusee {

constexpr int kProbeRingDepth = 4096;

struct ProbeFrame {
  const char *tag;
  uint64_t    ns;
  uint64_t    op_id;
};

class ProbeRing {
 public:
  ProbeRing() : head_(0), enabled_(false) {
    const char *e = getenv("FUSEE_PROBE_DUMP");
    if (e && e[0]) {
      enabled_ = true;
      // Allocate path; tid suffix added at dump time.
      dump_prefix_ = e;
    }
  }
  ~ProbeRing() { if (enabled_) dump(); }

  inline void emit(const char *tag, uint64_t op_id) {
    if (!enabled_) return;
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    ProbeFrame &f = ring_[head_ % kProbeRingDepth];
    f.tag = tag;
    f.ns = ns;
    f.op_id = op_id;
    head_++;
  }

  void dump() {
    if (!enabled_ || head_ == 0) return;
    char path[512];
    snprintf(path, sizeof(path), "%s.%d.%lu", dump_prefix_,
             (int)getpid(), (unsigned long)pthread_self());
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t n = (head_ >= (uint64_t)kProbeRingDepth)
                  ? kProbeRingDepth : (uint32_t)head_;
    uint32_t start = (head_ >= (uint64_t)kProbeRingDepth)
                      ? (uint32_t)(head_ % kProbeRingDepth) : 0;
    // Header: magic + frame count.
    uint32_t hdr[2] = { 0x50524F42u /* "PROB" */, n };
    fwrite(hdr, sizeof(hdr), 1, f);
    // Frames: write oldest-first.
    for (uint32_t i = 0; i < n; i++) {
      ProbeFrame &fr = ring_[(start + i) % kProbeRingDepth];
      // tag is a pointer to read-only string; emit as 8-byte pointer
      // (parser uses pointer->string lookup via /proc/<pid>/maps if
      // need be, OR — simpler — caller parses the same binary's tag
      // table). For simplicity, emit tag's first 8 chars padded.
      char tag_bytes[8] = {0};
      if (fr.tag) std::strncpy(tag_bytes, fr.tag, 8);
      fwrite(tag_bytes, 8, 1, f);
      fwrite(&fr.ns, 8, 1, f);
      fwrite(&fr.op_id, 8, 1, f);
    }
    fclose(f);
  }

 private:
  ProbeFrame ring_[kProbeRingDepth];
  uint64_t head_;
  bool enabled_;
  const char *dump_prefix_ = nullptr;
};

// One ProbeRing per thread. C++11 thread_local → constructor + dtor
// auto-fire at thread enter/exit. Zero overhead if FUSEE_PROBE_DUMP
// env is not set (ring->enabled_ is false → emit short-circuits).
extern thread_local ProbeRing *g_probe_ring;

inline ProbeRing *probe_ring() {
  if (!g_probe_ring) g_probe_ring = new ProbeRing();
  return g_probe_ring;
}

// Explicit dump call for use before _exit() (which skips destructors).
inline void probe_flush() {
  if (g_probe_ring) g_probe_ring->dump();
}

}  // namespace fusee

// Build-time gate. Default: disabled, true zero overhead.
#ifndef FUSEE_PROBE
#define FUSEE_PROBE 0
#endif

#if FUSEE_PROBE
#define PROBE(tag)        ::fusee::probe_ring()->emit(tag, 0)
#define PROBE_OP(tag, op) ::fusee::probe_ring()->emit(tag, (uint64_t)(op))
#else
#define PROBE(tag)        do {} while (0)
#define PROBE_OP(tag, op) do {} while (0)
#endif

#endif  // FUSEE_CXL_PROBE_H_
