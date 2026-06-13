#!/bin/bash
# G4: 4 co-located MN (b1 sid0..b4 sid3), CPU on NUMA1, mem interleaved (8GB > per-node 7GB hugepages).
set -u
for h in b1 b2 b3 b4; do ssh $h "pkill -f '[y]csb_test_server'" 2>/dev/null; done; sleep 2
i=0; for h in b1 b2 b3 b4; do ssh $h "cd ~/fusee_dbg && setsid numactl --cpunodebind=1 --interleave=0,1 stdbuf -oL ~/FUSEE/build/ycsb-test/ycsb_test_server $i >server.log 2>&1 </dev/null &" & i=$((i+1)); done; wait
sleep 6
ok=0; for h in b1 b2 b3 b4; do c=$(ssh $h "pgrep -af 'ycsb_test_server [0-9]'|wc -l"); [ "$c" -ge 1 ] && ok=$((ok+1)) || echo "MN $h DOWN"; done
[ $ok -eq 4 ] && echo "MN OK: 4/4 alive" || { echo "MN FAIL ($ok/4)"; exit 1; }
