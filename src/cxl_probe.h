#ifndef FUSEE_CXL_PROBE_H_
#define FUSEE_CXL_PROBE_H_

// iter-7A Phase 2: persistent mmap'd probe — captures FULL op history
// of a 200k-op cell. Replaces iter-6A's TLS 4096-frame ring (which
// only kept the last ~256 ops per worker, way too small to diagnose
// the 19 collapsed cells where the slowest ops occur randomly).
//
// Per-thread mmap'd file at $FUSEE_PROBE_DUMP/probe.{pid}.{tid},
// pre-allocated to 128 MB → 5.3M frames per thread (200k op × 16
// stages = 3.2M frames = 76 MB needs no overflow).
//
// Frame format (24 B):
//   char tag[8]   (zero-padded)
//   uint64 ns     (CLOCK_MONOTONIC nanoseconds)
//   uint64 op_id  (caller-supplied op identifier)
//
// File header (16 B):
//   uint32 magic = "PROB"
//   uint32 frame_capacity (= ring_bytes / 24)
//   uint64 frame_count (atomic; updated on emit)
//
// On overflow (extremely unlikely with 1.7× headroom), we emit one
// last "OVRFLOW" sentinel frame and stop.
//
// Design note: this is per-thread (not shared), so no atomic
// contention on the head index. Each thread has its own mmap'd
// region. Total disk: 132 threads × 128 MB = 17 GB on tmpfs (g3+g4
// /tmp is tmpfs 126 GB, plenty).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ctime>
#include <unistd.h>

namespace fusee {

constexpr std::size_t kProbeDumpBytes = 128ULL * 1024 * 1024;  // 128 MB
constexpr std::size_t kProbeHeaderBytes = 16;
constexpr std::size_t kProbeFrameBytes = 24;
constexpr std::size_t kProbeFrameCapacity =
    (kProbeDumpBytes - kProbeHeaderBytes) / kProbeFrameBytes;

struct ProbeHeader {
  uint32_t magic;       // "PROB" = 0x424F5250 (little-endian)
  uint32_t capacity;
  uint64_t count;
};

class ProbeRing {
 public:
  ProbeRing() : base_(nullptr), header_(nullptr), enabled_(false), overflowed_(false) {
    const char *e = getenv("FUSEE_PROBE_DUMP");
    if (!e || !e[0]) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s.%d.%lu", e,
             (int)getpid(), (unsigned long)pthread_self());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (ftruncate(fd, (off_t)kProbeDumpBytes) != 0) { close(fd); return; }
    void *p = mmap(nullptr, kProbeDumpBytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return;
    base_ = (uint8_t *)p;
    header_ = (ProbeHeader *)base_;
    header_->magic = 0x424F5250u;  // "PROB"
    header_->capacity = (uint32_t)kProbeFrameCapacity;
    header_->count = 0;
    enabled_ = true;
  }

  ~ProbeRing() { if (enabled_) flush(); }

  inline void emit(const char *tag, uint64_t op_id) {
    // iter-11A Phase 0.E fix: defensive null-guard on header_/base_.
    // Bimodal-cell investigation traced 8/210 sweep cells' 250×
    // throughput collapse to ReadReceiver SIGSEGV at this exact line
    // (cxl_probe.h:83 `header_->count`). enabled_ was true (so mmap
    // succeeded at constructor) but header_ was unmappable at emit()
    // time, which the prior code did not guard. Adding `!header_ ||
    // !base_` to the early-return condition prevents the crash;
    // probes are silently skipped when mapping is invalid (acceptable
    // — probes are diagnostic, not correctness-critical).
    if (!enabled_ || overflowed_ || !header_ || !base_) return;
    uint64_t idx = header_->count;
    if (idx >= kProbeFrameCapacity) {
      overflowed_ = true;
      // Best-effort sentinel.
      uint64_t sentinel_idx = kProbeFrameCapacity - 1;
      uint8_t *frame = base_ + kProbeHeaderBytes + sentinel_idx * kProbeFrameBytes;
      const char *o = "OVRFLOW";
      std::memcpy(frame, o, 8);
      return;
    }
    // iter-8A: RDTSCP + LFENCE for ordered timestamp.
    // RDTSCP waits for prior instructions to retire before reading TSC
    // → no OoO reorder around probe entry. LFENCE after blocks
    // following instructions from being moved before the read.
    // TSC_AUX → cpu_id (top 2 bytes of frame's "ns" field repurposed).
    unsigned a, d, c;
    asm volatile ("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
    asm volatile ("lfence");
    uint64_t cycles = ((uint64_t)d << 32) | a;
    // Pack: low 48 bits = TSC cycles (≈ 39 hours @ 2 GHz), high 16 = cpu_id
    uint64_t ns = (cycles & 0x0000FFFFFFFFFFFFULL) | ((uint64_t)(c & 0xFFFF) << 48);
    uint8_t *frame = base_ + kProbeHeaderBytes + idx * kProbeFrameBytes;
    char tag_buf[8] = {0};
    if (tag) std::strncpy(tag_buf, tag, 8);
    std::memcpy(frame, tag_buf, 8);
    std::memcpy(frame + 8, &ns, 8);
    std::memcpy(frame + 16, &op_id, 8);
    header_->count = idx + 1;
  }

  void flush() {
    if (base_) msync(base_, kProbeDumpBytes, MS_ASYNC);
  }

 private:
  uint8_t *base_;
  ProbeHeader *header_;
  bool enabled_;
  bool overflowed_;
};

extern thread_local ProbeRing *g_probe_ring;

inline ProbeRing *probe_ring() {
  if (!g_probe_ring) g_probe_ring = new ProbeRing();
  return g_probe_ring;
}

inline void probe_flush() {
  if (g_probe_ring) g_probe_ring->flush();
}

}  // namespace fusee

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
