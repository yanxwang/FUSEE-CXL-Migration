#!/bin/bash
# Apply G3 configs (run AFTER G2 done). 3MN=b1,b3,b4 ; 1CN=b2. data=idx=3, memory_num=3.
# server_data_len kept at 8GB (8589934592) — NOT changed.
set -eu
SRV='{
  "role": "SERVER", "conn_type": "IB", "server_id": 0, "udp_port": 2333,
  "memory_num": 3,
  "memory_ips": ["192.168.128.151", "192.168.128.153", "192.168.128.154"],
  "ib_dev_id": 1, "ib_port_id": 1, "ib_gid_idx": 0,
  "server_base_addr": "0x10000000", "server_data_len": 8589934592,
  "block_size": 67108864, "subblock_size": 512, "client_local_size": 1073741824,
  "num_replication": 3, "workload_run_time": 10,
  "main_core_id": 0, "poll_core_id": 1, "bg_core_id": 2, "gc_core_id": 3
}'
for h in b1 b3 b4; do ssh $h "mkdir -p ~/fusee_dbg; cat > ~/fusee_dbg/server_config.json" <<< "$SRV"; echo "$h server_config: $(ssh $h "grep -o 'memory_num.: [0-9]' ~/fusee_dbg/server_config.json")"; done
CLI='{
  "role": "CLIENT", "conn_type": "IB", "server_id": 3, "udp_port": 2333,
  "memory_num": 3,
  "memory_ips": ["192.168.128.151", "192.168.128.153", "192.168.128.154"],
  "ib_dev_id": 1, "ib_port_id": 1, "ib_gid_idx": 0,
  "server_base_addr": "0x10000000", "server_data_len": 8589934592,
  "block_size": 67108864, "subblock_size": 512, "client_local_size": 1073741824,
  "num_replication": 3, "num_idx_rep": 3, "num_coroutines": 8,
  "miss_rate_threash": 0.1, "workload_run_time": 10,
  "main_core_id": 0, "poll_core_id": 1, "bg_core_id": 2, "gc_core_id": 3
}'
ssh b2 "cat > ~/FUSEE/build/ycsb-test/client_config.json" <<< "$CLI"
ssh b2 "cat > ~/FUSEE/build/micro-test/client_config_2cn.json" <<< "$CLI"
echo "b2 client config: $(ssh b2 "grep -o 'memory_num.: [0-9]\|num_replication.: [0-9]\|num_idx_rep.: [0-9]' ~/FUSEE/build/ycsb-test/client_config.json | tr '\n' ' '")"
echo "G3 configs applied."
