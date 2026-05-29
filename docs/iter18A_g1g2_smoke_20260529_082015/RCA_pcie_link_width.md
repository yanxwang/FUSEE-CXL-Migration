# RCA: g1/g2 vs g3/g4 ~55% throughput shortfall — PCIe link width downgrade

**Date**: 2026-05-29
**Smoke test data**: [grid.csv](grid.csv)
**Hypothesis verified**: g1's Micron CXL devices are at **PCIe x8 (downgraded)**,
not x16. g3/g4 are believed to be x16 (could not re-verify, see below).

---

## 1. Throughput comparison (this RCA)

| Cell | g1/g2 (3-rep median) | g3/g4 iter-18A ref | ratio |
|---|---:|---:|---:|
| A: xhost_read T=32 N=0 V=1024 zipf-0.99 cache=0 | 0.634 Mops/s | 1.181 Mops/s | **54%** |
| B: xhost_read T=16 N=4 V=1024 zipf-0.99 cache=0 | 2.278 Mops/s | 4.066 Mops/s | **56%** |

Stable across reps (within 2%); not noise. CPU governor (powersave → performance)
changed only ±6% — ruled out.

## 2. PCIe link width evidence

`lspci -vv` on g1's CXL endpoints:

```
3b:00.0 CXL: XConn Technologies XC50256 (switch)
    LnkCap: Speed 32GT/s, Width x16
    LnkSta: Speed 32GT/s, Width x16             ✅ full

b0:00.0 CXL: Micron Technology Inc Device 6400  (-> dax0.0, shared with g2)
    LnkCap: Speed 32GT/s, Width x16
    LnkSta: Speed 32GT/s, Width x8 (downgraded) ❌ HALF

b1:00.0 CXL: Micron Technology Inc Device 6400  (-> dax1.0, g1 private)
    LnkCap: Speed 32GT/s, Width x16
    LnkSta: Speed 32GT/s, Width x8 (downgraded) ❌ HALF
```

g2's lspci shows only the CXL switch (x16); the Micron module reached via
switch must be one of g1's (b0:00.0) which is at x8 — so g2 inherits the
same bottleneck.

## 3. Bandwidth math

PCIe 5.0 (32 GT/s) per-direction theoretical:
- x16 = ~60-64 GB/s (after 8b/10b + protocol overhead, ~95 % of 64 GB/s raw)
- x8  = ~30-32 GB/s (half of above)

iter-18A summary cited g3/g4 CXL BW = **51.78 GB/s** (mlc-measured), consistent
with ~85 % efficiency on x16.

Expected g1/g2 with x8 Micron link: **~26 GB/s per direction** (half).
Observed throughput shortfall: ~55 % ✓ matches.

## 4. g3/g4 verification (incomplete)

Tried to rebuild iter-18A binary on g3/g4 to re-bench and confirm — failed
because PXE rootfs is missing `git` (`bash: line 9: git: command not found`
during `bootstrap_slave.sh` clone step). Will need:
```
ssh g3 'apt-get install -y git'
ssh g4 'apt-get install -y git'
```
then re-run bootstrap. Not done in this session.

g3 lspci confirmed: only the CXL switch (3b:00.0) is visible; the Micron
CXL device is **behind the switch** (different topology from g1's local
b0/b1:00.0). g3 dax0.0 = 512 GiB target_node=1 (vs g1 dax0.0 = 256 GiB
target_node=1) — different physical memory regions.

So g1+g2 and g3+g4 are **different physical CXL fabrics**, not the same
hardware with renamed labels.

## 5. Fix options

### Hardware
- **Re-seat g1's Micron CXL modules** in their PCIe slots. Link
  downgrade x16 → x8 typically indicates physical contact issue or BIOS
  bifurcation setting. After reseat, verify with `lspci -s b0:00.0 -vv | grep LnkSta`.
- Alternatively, check BIOS settings:
  - PCIe slot bifurcation (should be x16, not x8+x8)
  - PCIe link speed enforcement (some BIOS allow forcing Gen5 x16)
  - Disable any "Power Saving" PCIe link state settings (ASPM/CLKREQ).

### Workaround (if hardware can't be reseated)
- Accept g1/g2 baseline ≈ ½ of g3/g4 baseline.
- Update iter-18A summary to record the new reference numbers if
  future iter targets g1/g2 as the platform.
- mlc-measure g1/g2 CXL bandwidth to confirm the ~26 GB/s number,
  replacing the g3/g4 51.78 GB/s baseline.

## 6. Action items

- [ ] Physically re-seat g1's Micron CXL modules (or BIOS check)
- [ ] After reseat: `ssh g1 'lspci -s b0:00.0 -vv | grep LnkSta'` —
       confirm Width x16
- [ ] Install `mlc` (Intel Memory Latency Checker) on g1+g2 for
       direct CXL BW measurement; record in CLAUDE.md
- [ ] Install git on g3/g4 + rebootstrap, re-verify their PCIe link
       width is actually x16 (test the assumption, don't take iter-18A
       summary at face value)
- [ ] If g1/g2 confirmed permanently x8, update CLAUDE.md "Hardware
       baseline TBD" to "x8 link, ~26 GB/s BW; ~½ of g3/g4 ceiling"
