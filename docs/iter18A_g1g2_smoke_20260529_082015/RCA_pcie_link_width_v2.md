# RCA v2: testbed-wide CXL link width regression (NOT g1/g2-specific)

**Date**: 2026-05-29
**Supersedes**: [RCA_pcie_link_width.md](RCA_pcie_link_width.md) (v1, was wrong)

## Hypothesis tested (user-supplied)

1. **Topology**: g1/g2 and g3/g4 all connect via the same XConn switch to
   the same h3 server. h3 hosts 256 GiB (allocated to g1/g2) + 512 GiB
   (allocated to g3/g4) of Micron CXL memory modules.
2. **Link**: g1/g2 link width = ½ of g3/g4 link width, explaining the
   ~55 % throughput shortfall in FUSEE smoke (Exp 1 result on g1/g2 only).

## Experiments

### Exp A — Topology verification ✓ (premise 1 CONFIRMED)

`lspci | grep XConn` on all 4 hosts:

```
g1: 3b:00.0 CXL: XConn Technologies XC50256 (rev b2)
    + b0:00.0 CXL: Micron Technology Inc Device 6400 (rev 02)   (local enum)
    + b1:00.0 CXL: Micron Technology Inc Device 6400 (rev 02)   (local enum)
g2: 3b:00.0 CXL: XConn Technologies XC50256 (rev b2)
g3: 3b:00.0 CXL: XConn Technologies XC50256 (rev b2)
g4: 3b:00.0 CXL: XConn Technologies XC50256 (rev b2)
```

Identical XConn switch (model XC50256 rev b2) at identical BDF on all 4
hosts → consistent with same physical switch fabric to h3. g1 has the
two Micron modules enumerated under its `[0000:af]` root bridge (CXL
switch presents endpoints to g1 specifically); g2/g3/g4 reach the same
modules via switch routing without local PCI enumeration. Topology
premise (1) holds.

### Exp B — Link width via lspci -vv

```
g1's view of CXL endpoints:
  3b:00.0 XConn switch:           x16 cap, x16 sta  ✓ full
  b0:00.0 Micron Device 6400:     x16 cap, x8 sta   ❌ downgraded
  b1:00.0 Micron Device 6400:     x16 cap, x8 sta   ❌ downgraded

g2/g3/g4: only see the switch (3b:00.0 x16/x16) — Micron module's
LnkSta not visible because the endpoint isn't enumerated in their
local PCIe tree.
```

So the visible **lspci evidence is x8 downgrade on the (only) Micron
module enumeration we can see**. But this only confirms g1's view; we
cannot directly confirm whether the g3/g4 module behind the switch is
the same x8 or different.

### Exp C — Direct CXL read BW probe (cxl_bw_probe.c)

Custom tool: mmap /dev/dax0.0, flush every cacheline + mfence, then
sequentially load. Run with 1/8/16/32 parallel threads each on a
different CPU reading non-overlapping 1 GiB regions.

| host | single thread | 16 threads aggregate | 32 threads aggregate |
|---|---:|---:|---:|
| g1 | 6.51 GB/s | 26.66 GB/s | 26.63 GB/s |
| g2 | 6.47 GB/s | 26.64 GB/s | 26.64 GB/s |
| g3 | 6.64 GB/s | **26.97 GB/s** | **26.52 GB/s** |
| g4 | 6.64 GB/s | **26.83 GB/s** | **26.63 GB/s** |

**ALL 4 HOSTS SATURATE AT ~26.6 GB/s** (within 1.3 %). 32-thread same as
16-thread → BW ceiling reached.

PCIe 5.0 32 GT/s reference:
- x16 = ~60 GB/s per direction
- x8  = ~30 GB/s per direction

Measured 26.6 GB/s ≈ **x8 saturation efficiency ~85 %**. Consistent
with x8 link on the path to the Micron module.

### Exp D — FUSEE smoke re-run on g3/g4 today (vs iter-18A summary)

g3/g4 freshly bootstrapped today (2026-05-29), iter-18A binary
(`build-cxl-w1-v1024`, same flags as iter-18A originally used).

| Cell | iter-18A summary (2026-05-23) | g1/g2 (today) | **g3/g4 (today)** |
|---|---:|---:|---:|
| A: xhost_read T=32 N=0 V=1024 zipf cache=0 | 1.181 Mops | 0.634 | **0.651** |
| B: xhost_read T=16 N=4 V=1024 zipf cache=0 | 4.066 Mops | 2.278 | **2.155** |

**g3/g4 today reproduce g1/g2 today, not iter-18A historical.** Both
clusters at ~55 % of historical reference.

Data: [docs/iter18A_g3g4_smoke_20260529_085517/](../iter18A_g3g4_smoke_20260529_085517/)

## Conclusion — premise (2) FALSIFIED

Link width is **the same** on g1/g2 and g3/g4 today (all ~x8 effective,
~26.6 GB/s saturated read BW). The "g1/g2 = ½ g3/g4" hypothesis is
**wrong**.

The real story: **testbed-wide CXL link width regressed between
2026-05-23 (iter-18A measurement) and 2026-05-29 (today)**. Both
clusters dropped to about half their historical bandwidth.

iter-18A summary cited mlc = 51.78 GB/s on g3/g4 — that's consistent
with **x16 historical** (double the current measurement). Today's
26.6 GB/s = x8.

## Mechanism candidates

What could change CXL link width over a week without code changes?
1. **PCIe retraining / link health**: hot-plug or thermal events can
   downgrade Gen5 → Gen4 or x16 → x8. Recoverable via reboot in some
   BIOSes, persistent in others.
2. **BIOS update** on h3 (where the Micron modules physically live) that
   changed slot bifurcation defaults.
3. **Physical**: card movement, dust, vibration loosening Gen5 contacts.
4. **Firmware** update on XConn switch or Micron modules that
   renegotiated link parameters.
5. **PXE-rootfs kernel update** that loaded different CXL driver flags.

## Recommended next steps

1. **Capture the current actual link width on h3's side**:
   - h3 itself probably has lspci visibility into the Micron-to-switch
     internal link. Find out who has shell access to h3 and run
     `lspci -vv` there. That's where the truth lives.
2. **Reboot h3** and check if link comes back at x16. PCIe retraining
   often recovers from degraded state on a fresh boot. (Coordinate with
   whoever owns h3 — don't bounce someone else's server unannounced.)
3. **mlc-validate** on all 4 hosts to nail down the BW number to ±5%
   and replace the iter-18A summary baseline if today's value persists.
4. **Update iter-18A summary**: if testbed is permanently at the new
   baseline, the historical 1.18 / 4.07 Mops numbers are no longer the
   right comparison reference for future iters. Update CLAUDE.md hw
   baseline section accordingly.
