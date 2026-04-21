# Message Draft — Asking Colleague About c1/c2 IB Setup

Two versions below. Pick whichever fits your relationship and his communication style.

---

## Short version (Slack/DM-friendly)

> Hey, quick question about c1/c2 — I'm trying to run an RDMA workload between them but the IB port on both boxes is stuck in `State: Down, Physical: LinkUp`. `opensm` refuses to bind on either host because the NIC cap_mask has `IS_SM_DISABLED` set (I think BlueField-3 firmware default). `ib_write_bw` fails with "port state is Down". Have you ever gotten RDMA working between these two? Is there a managed switch between them running SM, or do you normally use Ethernet/RoCE mode? Would save me a lot of time. Thanks!

---

## Long version (email-friendly, with full context)

**Subject**: RDMA port stuck in Down state on c1/c2 — how is the IB fabric normally brought up?

> Hi [name],
>
> I'm trying to run an RDMA-based KV store (FUSEE, from FAST'23) on c1 and c2 for my research. Management network works fine, but I can't get the InfiniBand fabric up, and I suspect you've solved this before.
>
> **What I'm seeing**
>
> Both `mlx5_0` ports on c1 and c2 show:
> - `Physical state: LinkUp` (cable/optics fine)
> - `Logical state: Down`
> - `Rate: 100 Gb/s (4X EDR)` (currently in IB mode per `mlxconfig`)
> - `atomic_cap: ATOMIC_HCA` — exactly what I need for FUSEE
>
> `mlxlink` confirms the PHY is happy (`State: Active`, `Speed: IB-EDR`). But the logical port is Down, so anything needing MADs fails — `ibswitches`, `ibhosts`, and `ib_write_bw` all bail out with "port state is Down".
>
> **What I tried**
>
> - Loaded `ib_umad` — `/dev/infiniband/umad0` and `issm0` are present.
> - Started `opensm` on c1 (and c2 separately). Fails with:
>   ```
>   osm_vendor_rebind: ERR 5424: Unable to open port 0xc470bd0300b8638a
>   osm_sm_mad_ctrl_bind: ERR 3118: Vendor specific bind failed
>   Error from osm_opensm_bind (0x2A)   (IB_UNSUPPORTED)
>   ```
> - `cap_mask = 0xa751ec48` — bit 10 (`IS_SM_DISABLED`) is set. I think BlueField-3 firmware prohibits the host from acting as SM by default, expecting an external SM (on a managed switch).
>
> **My questions**
>
> 1. **How are c1 and c2 physically wired?** Back-to-back DAC, or through a managed IB switch (and maybe the CXL memory server)?
> 2. **If there's a managed switch, does its built-in SM run by default?** If not, how do you start it?
> 3. **Have you successfully run RDMA traffic between c1 and c2** (`ib_write_bw`, `perftest`, MPI, NCCL)? Any bringup recipe?
> 4. **Is either host usually configured for Ethernet/RoCE rather than native IB?** Currently `LINK_TYPE_P1/P2 = IB`. Switching to ETH would avoid the SM problem entirely — would that conflict with your workflow?
> 5. If the answer is "change to ETH and use RoCE," are you OK with me running `mlxconfig set LINK_TYPE_P1=ETH LINK_TYPE_P2=ETH` + `mlxfwreset`? It's a persistent firmware change so I want to check first.
>
> Happy to sync in person if easier. Full dump of `ibstat`, `ibv_devinfo -v`, `mlxconfig`, `mlxlink`, etc. is in `~/FUSEE/docs/c1_c2_ib_diagnostic.md` on my machine if you want to see it. Thanks!

---

## Attached diagnostic

Full tech dump: [c1_c2_ib_diagnostic.md](c1_c2_ib_diagnostic.md)

Commands to regenerate if needed:

```bash
ssh c1 "ibstat mlx5_0"
ssh c1 "ibv_devinfo -v | grep -E 'atomic_cap|state|phys_state|link_layer'"
ssh c1 "mlxconfig -d 16:00.0 query | grep LINK_TYPE_P"
ssh c1 "mlxlink -d 16:00.0 -p 1"
ssh c1 "cat /sys/class/infiniband/mlx5_0/ports/1/cap_mask"
```
