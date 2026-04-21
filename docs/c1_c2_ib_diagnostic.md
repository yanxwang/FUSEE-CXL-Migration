# c1 / c2 InfiniBand Fabric — Diagnostic Summary

Collected: 2026-04-19 23:15 UTC

## Goal
Run an RDMA-based KV store (FUSEE, FAST'23) on c1 and c2.
Needs: RDMA verbs + atomic operations (CAS). Hardware clearly supports it
(`atomic_cap = ATOMIC_HCA`), but the IB port never reaches `Active` state.

## Hardware

**c1**, **c2** — both have identical setup:
```
16:00.0 Infiniband controller: Mellanox Technologies MT43244 BlueField-3 integrated ConnectX-7 network controller (rev 01)
16:00.1 Infiniband controller: Mellanox Technologies MT43244 BlueField-3 integrated ConnectX-7 network controller (rev 01)
16:00.2 DMA controller: Mellanox Technologies MT43244 BlueField-3 SoC Management Interface (rev 01)

by-ibdev
by-path
issm0
issm1
umad0
umad1
uverbs0
uverbs1

eno1             UP             192.168.128.31/22 metric 100 fe80::7ec2:55ff:fe9e:9ee2/64 
eno2             DOWN           
ibs10f0          DOWN           192.168.200.1/24 
ibs10f1          DOWN           
tmfifo_net0      UNKNOWN        192.168.100.1/30 fe80::21a:caff:feff:ff02/64 
```

NIC: **NVIDIA BlueField-3 B3220** (integrated ConnectX-7), dual-port QSFP112
- Firmware: 32.47.1088 (latest)
- Capable of either 200 GbE *or* NDR200 IB (mlxconfig: LINK_TYPE_P1/P2 = IB)
- Physical link is up on `mlx5_0` (EDR 100Gb/s 4X) on both hosts

## The symptom

IB port is **Physical=LinkUp** but **Logical=Down** on both hosts.
Nothing that needs MADs works (`ibswitches`, `ibhosts`, `ib_write_bw`, ...).

### `ibstat mlx5_0` on c1
```
CA 'mlx5_0'
	CA type: MT41692
	Number of ports: 1
	Firmware version: 32.47.1088
	Hardware version: 1
	Node GUID: 0xc470bd0300b8638a
	System image GUID: 0xc470bd0300b8638a
	Port 1:
		State: Down
		Physical state: LinkUp
		Rate: 100
		Base lid: 65535
		LMC: 0
		SM lid: 0
		Capability mask: 0xa751ec48
		Port GUID: 0xc470bd0300b8638a
		Link layer: InfiniBand
---- mlx5_1 ----
CA 'mlx5_1'
	CA type: MT41692
	Number of ports: 1
	Firmware version: 32.47.1088
	Hardware version: 1
	Node GUID: 0xc470bd0300b8638b
	System image GUID: 0xc470bd0300b8638a
	Port 1:
		State: Down
		Physical state: LinkUp
		Rate: 100
		Base lid: 65535
		LMC: 0
		SM lid: 0
		Capability mask: 0xa751ec48
		Port GUID: 0xc470bd0300b8638b
		Link layer: InfiniBand
```

### `ibstat mlx5_0` on c2 (identical symptom)
```
CA 'mlx5_0'
	CA type: MT41692
	Number of ports: 1
	Firmware version: 32.47.1088
	Hardware version: 1
	Node GUID: 0xc470bd0300b866ce
	System image GUID: 0xc470bd0300b866ce
	Port 1:
		State: Down
		Physical state: LinkUp
		Rate: 100
		Base lid: 65535
		LMC: 0
		SM lid: 0
		Capability mask: 0xa751ec48
```

### Full device info (atomic_cap is good!)
```
hca_id:	mlx5_0
	fw_ver:				32.47.1088
	vendor_part_id:			41692
	hw_ver:				0x1
	board_id:			MT_0000000965
	device_cap_flags:		0x21361c36
	max_qp_rd_atom:			16
	atomic_cap:			ATOMIC_HCA (1)
	device_cap_flags_ex:		0x3000005021361C36
			state:			PORT_DOWN (1)
			active_width:		4X (2)
			active_speed:		25.0 Gbps (32)
			phys_state:		LINK_UP (5)
hca_id:	mlx5_1
	fw_ver:				32.47.1088
	vendor_part_id:			41692
	hw_ver:				0x1
	board_id:			MT_0000000965
	device_cap_flags:		0x21361c36
	max_qp_rd_atom:			16
	atomic_cap:			ATOMIC_HCA (1)
	device_cap_flags_ex:		0x3000005021361C36
			state:			PORT_DOWN (1)
			active_width:		4X (2)
			active_speed:		25.0 Gbps (32)
			phys_state:		LINK_UP (5)
```

### Port cap_mask — c1
```
cap_mask: 0xa751ec48
  state:
1: DOWN
  phys_state:
5: LinkUp
  link_layer:
InfiniBand
  rate:
100 Gb/sec (4X EDR)
```
**bit 10 (0x400 — IS_SM_DISABLED) is set** → firmware prohibits this host from acting as SM.

### Port cap_mask — c2 (same)
```
cap_mask: 0xa751ec48
  state:
1: DOWN
  phys_state:
5: LinkUp
```

### Current mlxconfig
```
        NUM_OF_VFS                                  16                  
        SRIOV_EN                                    True(1)             
        KEEP_ETH_LINK_UP_P1                         True(1)             
        KEEP_IB_LINK_UP_P1                          False(0)            
        KEEP_ETH_LINK_UP_P2                         True(1)             
        KEEP_IB_LINK_UP_P2                          False(0)            
        LINK_TYPE_P1                                IB(1)               
        LINK_TYPE_P2                                IB(1)               
```

### mlxlink (PHY is fine)
```

Operational Info
----------------
State                              : Active 
Physical state                     : LinkUp 
Speed                              : IB-EDR 
Width                              : 4x 
FEC                                : Standard LL RS-FEC - RS(271,257) 
Loopback Mode                      : No Loopback 
Auto Negotiation                   : ON 

Supported Info
--------------
Enabled Link Speed                 : 0x00000031 (EDR,FDR,SDR) 
Supported Cable Speed              : 0x0000003f (EDR,FDR,FDR10,QDR,DDR,SDR) 

Troubleshooting Info
--------------------
Status Opcode                      : 0 
Group Opcode                       : N/A 
Recommendation                     : No issue was observed 

Tool Information
----------------
Firmware Version                   : 32.47.1088 
amBER Version                      : 5.75 
MFT Version                        : 4.34.1-10 
```

## What I've tried

### 1. Load `ib_umad` kernel module
Worked. `/dev/infiniband/umad0` and `issm0` are present:
```
ls /dev/infiniband/
→ by-ibdev  by-path  issm0  issm1  umad0  umad1  uverbs0  uverbs1
```

### 2. Run `opensm` on c1
Fails immediately:
```
-------------------------------------------------
OpenSM 5.25.1.MLNX20251030.e3791a47
-I- Set parameter "guid" to "0xc470bd0300b8638a" by command line
-I- Configuration loaded
 Log File: /var/log/opensm.log
-------------------------------------------------
OpenSM 5.25.1.MLNX20251030.e3791a47

Entering DISCOVERING state


Error from osm_opensm_bind (0x2A)
Perhaps another instance of OpenSM is already running
Exiting SM
```
The critical line is `ERR 5424: Unable to open port` and `osm_opensm_bind (0x2A)` —
IB_UNSUPPORTED. This is consistent with the SM_DISABLED cap bit above.

### 3. Try a direct RDMA test anyway
```
 Port number 1 state is Down
 Couldn't set the link layer
 Couldn't get context for the device
 WARNING: BW peak won't be measured in this run.
```
Refuses because the logical port is Down.

### 4. dmesg mlx5 lines (no errors, driver loaded clean)
```

```

## Questions for you

1. **How are c1 and c2 physically wired?** Back-to-back with a DAC, or
   through a managed IB switch (and maybe the CXL memory server)?

2. **If there's a managed switch, is its built-in SM running?** If not, how
   do you normally start it?

3. **Have you successfully run RDMA traffic between c1 and c2** (`ib_write_bw`,
   `perftest`, MPI, NCCL, ...)? If yes, any specific bringup sequence you use?

4. **Is either host usually configured for Ethernet/RoCE rather than native IB?**
   (`LINK_TYPE_P1/P2 = IB` currently on both.) Switching to ETH would sidestep
   the whole SM problem — would that be acceptable for your workflow?

5. **Is the SM_DISABLED bit something you deliberately set**, or is it the
   factory default and you rely on an external SM? If I need to flip it, is
   there a preferred way?

Any pointers appreciated. I have root on both hosts and am comfortable
running mlxconfig / mlxfwreset if you confirm that's the right path.

---

## Context on what I need (for reference)

FUSEE is a disaggregated KV store that relies on:
- RDMA Read / Write (done) ✅
- **RDMA CAS (`IBV_WR_ATOMIC_CMP_AND_SWP`)** — this is the critical one.
  Our previous cluster (Connect-IB MT27600) reported `atomic_cap = ATOMIC_NONE`
  and FUSEE hung in `ibv_poll_cq` on every CAS. BlueField-3 reports
  `ATOMIC_HCA`, which is why I'm keen to move to c1/c2.

Works either over native IB or over RoCE v2 — doesn't matter to the
application. Whichever is easier to stand up.
