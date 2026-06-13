#!/bin/bash
# G3: restart 3 memory nodes (b1 sid0, b3 sid1, b4 sid2). Parallel launch (lone server aborts).
set -u
for h in b1 b3 b4; do ssh $h "pkill -f '[y]csb_test_server'" 2>/dev/null; done
sleep 2
ssh b1 "cd ~/fusee_dbg && setsid stdbuf -oL ~/FUSEE/build/ycsb-test/ycsb_test_server 0 >server.log 2>&1 </dev/null &" &
ssh b3 "cd ~/fusee_dbg && setsid stdbuf -oL ~/FUSEE/build/ycsb-test/ycsb_test_server 1 >server.log 2>&1 </dev/null &" &
ssh b4 "cd ~/fusee_dbg && setsid stdbuf -oL ~/FUSEE/build/ycsb-test/ycsb_test_server 2 >server.log 2>&1 </dev/null &" &
wait
sleep 5
ok=0
for h in b1 b3 b4; do c=$(ssh $h "pgrep -af 'ycsb_test_server [0-9]'|wc -l"); [ "$c" -ge 1 ] && ok=$((ok+1)) || echo "MN $h DOWN"; done
[ $ok -eq 3 ] && echo "MN OK: 3/3 alive" || { echo "MN FAIL ($ok/3)"; exit 1; }
