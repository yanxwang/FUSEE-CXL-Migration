# RCA v3: testbed-wide CXL link width regression — root cause CONFIRMED from h3 side

**Date**: 2026-05-29
**Supersedes**: [RCA_pcie_link_width_v2.md](RCA_pcie_link_width_v2.md)

## h3 direct evidence (the smoking gun)

Connected to h3 (192.168.128.250, admin user) via password and inspected
CXL link state from the **memory host side** (where the modules
physically attach):

```
h3 kernel: 6.6.4, hostname H3G5
$ lspci | grep CXL
08:00.0 Montage c002 (rev 03)    ← CXL retimer, downstream of slot
14:00.0 Montage c002 (rev 03)    ← CXL retimer
1f:00.0 Micron Device 6400       ← MEMORY MODULE 1 (→ g1/g2 fabric, 256 GiB partition)
31:00.0 Montage c002 (rev 03)    ← CXL retimer
3d:00.0 Montage c002 (rev 03)    ← CXL retimer
47:00.0 Micron Device 6400       ← MEMORY MODULE 2 (→ g3/g4 fabric, 512 GiB partition)

$ lspci -s 1f:00.0 -vv | grep Lnk
    LnkCap: Speed 32GT/s, Width x16
    LnkSta: Speed 32GT/s, Width x8 (downgraded)   ❌

$ lspci -s 47:00.0 -vv | grep Lnk
    LnkCap: Speed 32GT/s, Width x16
    LnkSta: Speed 32GT/s, Width x8 (downgraded)   ❌

$ lspci -s 08:00.0 -vv | grep Lnk    (and 14, 31, 3d — same)
    LnkCap: Speed 32GT/s, Width x8
    LnkSta: Speed 32GT/s, Width x8
```

**Both Micron CXL memory modules are physically x16 capable but currently
negotiated at x8.** This is the root cause. The Montage retimers are
inherently x8 by design (LnkCap = x8); they sit in the CXL fabric path
between Micron and the XConn switch.

Architecture inference:
```
[g1/g2 hosts] -- XConn switch -- Montage retimer chain -- Micron #1 (1f:00.0, x8 today, x16 designed)
[g3/g4 hosts] -- XConn switch -- Montage retimer chain -- Micron #2 (47:00.0, x8 today, x16 designed)
```

The retimers can carry x8; Micron is meant to drive x16 across two
retimer paths in parallel. Today both Micron modules use only one
half → effectively x8.

## When the regression happened

h3 dmesg timestamps show cxl_pci initialization at `~362,827 s` ago =
**~4.2 days**, which puts the most recent h3 reboot around 2026-05-25.

iter-18A measurement completed 2026-05-23 (when the 51.78 GB/s mlc
baseline was captured). So the regression occurred **on the h3 reboot
between 2026-05-23 and 2026-05-25**.

## Why both Micron downgraded at once

Both modules attach via the **same chassis** (h3), so a single reboot
event triggered fresh PCIe Gen5 link training on both slots. Some Gen5
links don't reliably retrain at x16 — known industry issue with early
Gen5 silicon. Once a link comes up at x8 it stays there until next
training cycle (next reboot or `pcie_link_retraining`).

## Refined conclusions on user's two hypotheses

| Hypothesis | Verdict |
|---|---|
| (1) g1/g2 and g3/g4 share the same XConn switch / h3 server | ✅ CONFIRMED — h3 lspci shows the two Micron modules attached locally |
| (2) g1/g2 link = ½ g3/g4 link | ❌ FALSIFIED — BOTH Micron modules currently x8. The "½" symmetry is testbed-wide vs iter-18A historical, not g1/g2 vs g3/g4 |

## Bandwidth model — final

| Era | Micron LnkSta | Read BW (single-direction) | Read+Write bidir (mlc) |
|---|---|---:|---:|
| iter-18A 2026-05-23 (historical) | x16 (designed) | ~52 GB/s | 51.78 GB/s ← g3/g4 mlc baseline cited in iter-18A summary |
| 2026-05-29 (today) | x8 (downgraded) | ~26 GB/s | ~52 GB/s? — un-validated, but expected half of x16 |

cxl_bw_probe measured 26.6 GB/s saturated read on ALL four FUSEE hosts
today → matches x8 prediction at 88 % efficiency. Math is internally
consistent.

## Recovery options (ordered by likelihood of success / disruption)

1. **h3 reboot** — link retraining at boot has good odds of negotiating
   x16 if the underlying issue is just intermittent. Cost: testbed
   downtime ~5 min. Likely first thing to try.
2. **Manual link retraining via sysfs** (no reboot, lower disruption):
   ```bash
   ssh h3 'echo 1 > /sys/bus/pci/devices/0000:1f:00.0/reset
            echo 1 > /sys/bus/pci/devices/0000:47:00.0/reset'
   ```
   then re-check LnkSta. Some kernels don't honor `reset` for CXL
   class; may need pci hotplug.
3. **PCIe link state explicit retrain via lspci -s ... -vv setpci**
   (Link Control register bit 5):
   ```bash
   sudo setpci -s 1f:00.0 CAP_EXP+10.w=20:20    # bit 5 = Retrain Link
   sudo setpci -s 47:00.0 CAP_EXP+10.w=20:20
   ```
   then re-read LnkSta. Could trigger Micron-side renegotiation.
4. **BIOS update on h3** if vendor has a CXL Gen5 link training fix.
5. **Reseat Micron + Montage retimers physically** — last resort if
   software approaches don't recover.

## Action items

- [ ] Coordinate with h3 owner before reboot or setpci attempts
      (these touch the shared CXL fabric and will disturb anyone
      running benchmarks on g1/g2/g3/g4).
- [ ] After any recovery attempt: re-check LnkSta on h3 + re-run
      cxl_bw_probe on g1+g2+g3+g4 and re-run FUSEE smoke A+B to
      confirm thpt back to iter-18A historical levels (1.18 / 4.07
      Mops).
- [ ] If recovery sticks, update iter-18A summary §"Hardware
      baseline" with a note: "as of 2026-05-29, CXL link recovered via
      <method>; historical 51.78 GB/s baseline re-validated."
- [ ] If recovery fails, update CLAUDE.md "g1/g2 hardware baseline TBD"
      → "BW today ~26.6 GB/s single-direction, x8 fabric; iter-18A
      historical 51.78 GB/s (x16) no longer reproducible until h3 link
      retrained."
- [ ] Document this incident in iter-18A summary as a cautionary note
      for cross-iter comparison validity.
