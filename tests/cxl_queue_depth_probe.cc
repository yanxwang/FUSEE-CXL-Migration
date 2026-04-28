// iter-2A-revised — queue-depth probe.
//
// Standalone binary that attaches to an already-running runner's
// LocalAggregatorRegion (via SHM_PATH env or /proc/<pid>/fd lookup
// — for now, just instructs to be embedded in the runner). For
// in-process Phase 7 decomp, simpler approach: have the runner spawn
// a probe thread that samples queue/ring depths every 100ms and
// writes to a CSV file. This binary instead exposes a one-shot
// snapshot of an EXTERNALLY visible region — useful only when the
// runner's region pointer is exposed via shared file or known offset.
//
// For iter-2A-revised Phase 7 we don't need cross-process probing —
// the in-process probe (added to cxl_ycsb_runner.cc) is sufficient.
// This file is a stub kept for symmetry with the design doc; the
// real probe logic lives in the runner's own diagnostics path.

#include <cstdio>

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  fprintf(stderr,
          "cxl_queue_depth_probe: stub — Phase 7 queue-depth probe\n"
          "is integrated into cxl_ycsb_runner.cc (FUSEE_QUEUE_DEPTH_CSV env\n"
          "writes a CSV; sample every 100ms via background thread).\n");
  return 0;
}
