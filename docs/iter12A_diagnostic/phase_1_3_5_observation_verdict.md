# Phase 1.3.5 Observation Verdict (iter-12A bimodal RCA)

**Date**: 2026-05-16
**Reference cell**: `workloada T=4 cache=off kv=512` (cross-iter stable: BIMODAL in both iter-10A and iter-11A 5-rep verifies)
**Observation methodology**: per `task_plan_iter12A.md §1.0–1.3`, observe-first (no hypothesis before data)

---

## Sample / data summary

### Phase 0.C — bimodal reproduces strongly

20-rep baseline on reference cell:

| Class | Count | thpt range (Mops/s) | wall (s) | mean op latency |
|---|---:|---|---|---|
| WIN | 9 / 20 | 1.32–1.54 | 0.030–0.040 | ~3.5 µs (healthy) |
| HARD-COLLAPSE | 8 / 20 | 0.0058–0.0059 | 8.51–8.66 | ~680 µs |
| MID-COLLAPSE | 1 / 20 | 0.349 | 0.143 | ~46 µs |
| TIMEOUT (90s) | 2 / 20 | 0 | NA | runaway |

Two distinct collapse tiers confirmed across 20 reps. The exact quantization (0.0058 / 0.349 thpt values) **strongly suggests a deterministic timeout mechanism** (not stochastic).

### Phase 1.1 — perf record sampling (16 reps, 4 collapsed)

Top functions in a COLLAPSE rep (6K samples, 8.5 s wall):

| Symbol | % | What it is |
|---|---:|---|
| `ReadSender::sender_loop_dispatch<1>` | 16.95% | idle-polling SPSC ring |
| `WriteSender::sender_loop_dispatch<0>` | 16.93% | idle-polling SPSC ring |
| `InvalSender::sender_loop_dispatch<2>` | 16.93% | idle-polling SPSC ring |
| `InvalReceiver::inval_receiver_loop` | 11.28% | idle-polling SPSC ring |
| `ReadReceiver::read_receiver_loop` | 11.16% | idle-polling SPSC ring |
| `WriteReceiver::write_receiver_loop` | 10.97% | idle-polling SPSC ring |
| **`CxlKvStoreA::forward_write_direct`** | **10.67%** | **only worker function in top** |
| (everything else) | < 1% each | load file parse / main / memchr |

**Notable absences**:
- No `forward_read_direct` (read path is not the issue)
- No `pthread_mutex_lock` / `futex_wait` / `__lll_lock_wait` (NOT a lock contention issue → **rules out V2-style lock state machine**)
- No `sched_yield` / `clone3 → fork` (no scheduling-API calls in worker hot path)

Compared to WIN rep (385 samples in 0.03 s, all of it in `load_ops_from_file` + setup), the collapse profile is dominated by ring polling.

### Phase 1.3 — gdb -batch worker process snapshot (16 reps)

For every collapse / timeout rep where workers were sampled mid-trans phase (5 reps captured):

**Parent worker (client_id=0, PID = launch pid)**:
- State: `R (running)`
- Thread 1 backtrace (consistent across all 5 reps, across all 3 sample timepoints t=3.0s / 5.0s / 7.0s within the same rep):
  ```
  #0  fusee::generic_spin_wait<WriteEntry>           at cxl_kv_ops_A.cc:74
  #1  CxlKvStoreA::forward_write_direct              at cxl_kv_ops_A.cc:1182
  #2  main
  ```
- `op_id` at sample t=3s, 5s, 7s on same rep: **progressing** (different op_id per sample), op_id delta ≈ 175 ops/sec.

**3 forked child workers (client_id 1, 2, 3)**:
- State: `Z (zombie)` — already exited at t=3.0s
- Children finished their trans share in < 3s wall (i.e., normally / healthy)

Across all 5 collapse-or-timeout reps captured, this stack/state pattern is identical. **The "bimodal" run is actually: one stuck parent, three healthy children**.

---

## What we observed (synthesized, cited)

1. **Parent worker is the only stuck worker** (Phase 1.3: 5/5 reps confirm parent R-state + 3 children Z-state at all sample timepoints).
2. **Parent is stuck in `generic_spin_wait` line 74** (`if (resp == op_id)` — i.e., waiting for `resp_op_id` to match `op_id` of its in-flight write).
3. **Parent IS progressing through ops**, not deadlocked on one op (Phase 1.3: op_id changes between samples on the same rep, ~175 ops/sec).
4. **Per-op latency math fits 5-ms timeout mechanism** (Phase 0.C wall × thpt math):
   - HARD-COLLAPSE: 680 µs / op = 13.6% of ops hit the 5 ms timeout
   - MID-COLLAPSE: 46 µs / op = 0.85% timeout rate
   - WIN: 3.5 µs / op = 0% timeout
5. **The 5 ms timeout** is `kBudgetUs = 5000` at `cxl_kv_ops_A.cc:68`. On timeout, `generic_spin_wait` returns `-11` and `req_op_id` is reset to 0.
6. **No lock contention** in collapse rep — perf shows zero futex / mutex symbols.
7. **No CPU-idle blocked-on-syscall pattern** — workers are `R`, not `S` or `D`. CPU is busy.
8. **No `forward_read_direct` in top** — read path is NOT the bottleneck during collapse.
9. **Receiver loops have HoL-block bug** at `cxl_kv_ops_A.cc:1340 / 1638 / 1675` (`if (op_id == 0) break;`) — when a producer is mid-publish (fetch_add done, req_op_id not written yet), receiver bails out and may stall waiting for the gap to heal.
10. **HARD-COLLAPSE has 13.6% timeout rate × 5 ms each → parent wall ≈ 8.5 s**, matching observed wall.

---

## Verdict (per Phase 1.3.5 framework)

### V1 (receiver preempted > 10ms) — **MOSTLY NO**

CFS time-slice on this kernel is 1–3 ms. Pure CFS-preempt explanation would require >5 ms quanta which is atypical. Per QR2 user prior, scheduling-as-root-cause is unlikely. Plus, `R` state of the parent rules out blocking syscalls.

However, the parent worker IS on CPU 0 (`pthread_setaffinity_np(client_id=0)` at `protocol_a_ycsb.cc:316`), which on Linux **often handles disproportionate IRQ / RCU / ksoftirqd work**. Sub-millisecond IRQ jitter on CPU 0 could lengthen the per-publish path enough to be exposed by the receiver's HoL block (V2). So V1 has a **secondary** contribution but is not the primary trigger.

### V2 (ring state machine HoL block) — **PRIMARY YES**

The exact code defect: `if (op_id == 0) break;` in all three receiver loops (write at line 1638, read at line 1675, inval at line 1340). This is a **HEAD-OF-LINE BLOCK**:
- Producer A `fetch_add(1)` → claims tpos=N
- Producer B (or A itself, after preempt-then-publish) → also claims tpos=N+k
- Receiver reads `tail = N+k+1`. Slot at `head=N` still has `req_op_id == 0` (A hasn't published yet, or has been briefly delayed).
- Receiver **breaks out**, sets `ring->head = N` (no advance).
- Receiver loops back via outer `while (!stop_)`. Each retry costs ~1 µs (flush+fence). If A's publish takes 1–10 µs (typical), gap heals within outer loop. If A's publish takes > 5 ms (rare but possible under CPU 0 noise), generic_spin_wait of slot at N (and any slot N+k behind it) times out → 5 ms penalty per affected op.

The combination of (V1's contribution: CPU 0 occasionally delayed publish) × (V2's amplification: receiver HoL block exposes that delay as a 5-ms generic_spin_wait timeout) produces the observed bimodal.

### V3 (worker spinning unintended path) — **NO**

perf top is `forward_write_direct`, which IS the intended write path. Parent is doing what it should — just timing out.

### V4 (data noise) — **NO**

Observations are clear and consistent across 5+ collapse reps.

---

## Decision per Phase 1.5 RAP → Phase 1.6 fix design

**Proceed to Phase 1.5 RAP** with:

- **Primary root cause**: receiver HoL block in 3 receiver loops at `cxl_kv_ops_A.cc:1340 / 1638 / 1675` (V2)
- **Secondary amplifier**: CPU 0 OS noise (V1) — workers on CPU 0 occasionally need > 5 ms to publish; combined with V2, this 5 ms gap propagates into worker timeouts

**Fix design (narrow-targeted, per C17)**:

1. **Primary fix (V2)** — receiver gap-tolerance budget at the 3 break-on-zero sites:
   - Change `if (op_id == 0) break;` to a budgeted in-place spin (e.g., 200 µs budget per gap encounter, ~256 paused load iterations) before bailing.
   - Modifies 3 functions in `cxl_kv_ops_A.cc`: `inval_receiver_loop` (line 1340), `write_receiver_loop` (line 1638), `read_receiver_loop` (line 1675).
   - **Within C17 budget** (3 functions, 1 file).

2. **Secondary fix (V1)** — bump `generic_spin_wait` timeout from 5 ms to 50 ms (`kBudgetUs = 5000 → 50000` at line 68):
   - Gives worker more breathing room when receiver-side processing is briefly delayed.
   - Modifies 1 constant in 1 helper function in same file.
   - **Within C17 budget**.

3. **L1-only `SCHED_FIFO` (per QR2)** — **DEFER** unless (1)+(2) insufficient:
   - QR2 says L1 only and "scheduling stall极不可能". Sticking with QR2 prior.

**Skipped phases** (per QR1 default):
- Phase 1.2 ftrace sched_switch — not needed, evidence already sufficient
- Phase 1.4 event-trace code — verdict-gate skip (V2 strong evidence)

**Hash-diff requirement** (per C9 / C13): after Phase 1.6 fix, 20-cell G1 hash-diff must PASS, since the fix touches strict-A invariant code (write/read/inval ring dequeue ordering).

---

## Open question on cause of asymmetric parent stuck

We have NOT directly observed CPU-0 IRQ jitter (would require ftrace from Phase 1.2 which we skip per QR1). The "CPU 0 noisy → parent slow → V2 exposes it" model is **inferred**, not measured. If Phase 1.6 fix (1)+(2) does not reduce bimodal count below 8 (gate-12 PASS), this open question must be revisited via Phase 1.2 ftrace + L2/L3 SCHED_FIFO escalation per QR2.
