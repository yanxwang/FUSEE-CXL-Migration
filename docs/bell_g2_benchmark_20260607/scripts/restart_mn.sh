#!/bin/bash
# Restart the 2 G1 memory nodes (b1 sid0, b3 sid1) cleanly.
# KEY: both MUST launch near-simultaneously — a lone server without a live
# peer aborts during inter-server RDMA init. Launch in parallel, then verify.
set -u
ssh b1 "pkill -f '[y]csb_test_server'" 2>/dev/null
ssh b3 "pkill -f '[y]csb_test_server'" 2>/dev/null
sleep 2
ssh b1 "cd ~/fusee_dbg && setsid stdbuf -oL ~/FUSEE/build/ycsb-test/ycsb_test_server 0 >server.log 2>&1 </dev/null &" &
ssh b3 "cd ~/fusee_dbg && setsid stdbuf -oL ~/FUSEE/build/ycsb-test/ycsb_test_server 1 >server.log 2>&1 </dev/null &" &
wait
sleep 4
a=$(ssh b1 "pgrep -af 'ycsb_test_server [0-9]' | wc -l")
b=$(ssh b3 "pgrep -af 'ycsb_test_server [0-9]' | wc -l")
if [ "$a" -ge 1 ] && [ "$b" -ge 1 ]; then
  echo "MN OK: b1=alive b3=alive"
else
  echo "MN FAIL: b1_cnt=$a b3_cnt=$b"; ssh b1 "tail -3 ~/fusee_dbg/server.log"; ssh b3 "tail -3 ~/fusee_dbg/server.log"; exit 1
fi
