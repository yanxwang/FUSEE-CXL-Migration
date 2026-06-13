| file | line | enclosing function | site |
|---|---|---|---|
| `cxl_kv_blockpool.cc` | 96 | `CxlKvBlockPool::attach` | `flush_region(base_, sizeof(Header) +` |
| `cxl_kv_blockpool.cc` | 98 | `CxlKvBlockPool::attach` | `store_fence();` |
| `cxl_kv_blockpool.cc` | 102 | `CxlKvBlockPool::attach` | `flush_line(&hdr->magic);` |
| `cxl_kv_blockpool.cc` | 103 | `CxlKvBlockPool::attach` | `full_fence();` |
| `cxl_kv_blockpool.cc` | 118 | `CxlKvBlockPool::attach` | `flush_line(&cursors_[h].bump);` |
| `cxl_kv_blockpool.cc` | 120 | `CxlKvBlockPool::attach` | `full_fence();` |
| `cxl_kv_blockpool.cc` | 183 | `CxlKvBlockPool::write` | `flush_line(p);` |
| `cxl_kv_blockpool.cc` | 187 | `CxlKvBlockPool::write` | `store_fence();` |
| `cxl_kv_blockpool.cc` | 203 | `CxlKvBlockPool::read` | `flush_line(p);` |
| `cxl_kv_blockpool.cc` | 206 | `CxlKvBlockPool::read` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 289 | `publish_slot_cow` | `flush_line(slot);` |
| `cxl_kv_ops_A.cc` | 290 | `publish_slot_cow` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 297 | `retire_slot` | `flush_line(slot);` |
| `cxl_kv_ops_A.cc` | 298 | `retire_slot` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 332 | `generic_spin_wait` | `flush_line((void *)&e->resp_op_id);` |
| `cxl_kv_ops_A.cc` | 340 | `generic_spin_wait` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 341 | `generic_spin_wait` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 352 | `generic_spin_wait` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 353 | `generic_spin_wait` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 413 | `CxlKvStoreA::attach` | `flush_line(&buckets_[b]);` |
| `cxl_kv_ops_A.cc` | 414 | `CxlKvStoreA::attach` | `flush_line((char *)&buckets_[b] + 64);` |
| `cxl_kv_ops_A.cc` | 416 | `CxlKvStoreA::attach` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 855 | `CxlKvStoreA::enable_write_ring` | `flush_region(wr, write_ring_matrix_bytes());` |
| `cxl_kv_ops_A.cc` | 857 | `CxlKvStoreA::enable_write_ring` | `flush_region(fs, forward_staging_matrix_bytes());` |
| `cxl_kv_ops_A.cc` | 858 | `CxlKvStoreA::enable_write_ring` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 905 | `CxlKvStoreA::enable_read_ring` | `flush_region(rr, read_ring_matrix_bytes());` |
| `cxl_kv_ops_A.cc` | 907 | `CxlKvStoreA::enable_read_ring` | `flush_region(rs, read_staging_matrix_bytes());` |
| `cxl_kv_ops_A.cc` | 908 | `CxlKvStoreA::enable_read_ring` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1183 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 1184 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1200 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1201 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1212 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `flush_line((void *)(staging + off));` |
| `cxl_kv_ops_A.cc` | 1214 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `store_fence();  // ⭐ critical: staging must be CXL-visible before req` |
| `cxl_kv_ops_A.cc` | 1225 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1226 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `store_fence();  // ⭐ critical: req must be CXL-visible before next slo...` |
| `cxl_kv_ops_A.cc` | 1240 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `flush_line((void *)&e->resp_op_id);` |
| `cxl_kv_ops_A.cc` | 1241 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1247 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1248 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1263 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1268 | `CxlKvStoreA::write_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1310 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 1311 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1324 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1325 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1336 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1337 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `store_fence();  // ⭐ slot N must commit before slot N+1` |
| `cxl_kv_ops_A.cc` | 1350 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `flush_line((void *)&e->resp_op_id);` |
| `cxl_kv_ops_A.cc` | 1351 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1370 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1371 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1385 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1390 | `CxlKvStoreA::read_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1423 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 1424 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1437 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1438 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1447 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1448 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `store_fence();  // ⭐ slot N must commit before slot N+1` |
| `cxl_kv_ops_A.cc` | 1461 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `flush_line((void *)&e->resp_op_id);` |
| `cxl_kv_ops_A.cc` | 1462 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1468 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1469 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1483 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1488 | `CxlKvStoreA::inval_sender_drain_dst_v2_unused` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1691 | `CxlKvStoreA::forward_write_direct` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 1692 | `CxlKvStoreA::forward_write_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1705 | `CxlKvStoreA::forward_write_direct` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 1747 | `CxlKvStoreA::forward_write_direct` | `flush_line((void *)(staging + off));` |
| `cxl_kv_ops_A.cc` | 1749 | `CxlKvStoreA::forward_write_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1783 | `CxlKvStoreA::forward_write_direct` | `flush_line((void *)&e->req_op_id);  // publish cacheline 1` |
| `cxl_kv_ops_A.cc` | 1784 | `CxlKvStoreA::forward_write_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1822 | `CxlKvStoreA::send_invalidate_direct` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 1823 | `CxlKvStoreA::send_invalidate_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1830 | `CxlKvStoreA::send_invalidate_direct` | `flush_line((void *)e);` |
| `cxl_kv_ops_A.cc` | 1831 | `CxlKvStoreA::send_invalidate_direct` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1841 | `CxlKvStoreA::send_invalidate_direct` | `flush_line((void *)e);` |
| `cxl_kv_ops_A.cc` | 1842 | `CxlKvStoreA::send_invalidate_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1855 | `CxlKvStoreA::send_invalidate_direct` | `flush_line((void *)&e->resp_op_id);` |
| `cxl_kv_ops_A.cc` | 1856 | `CxlKvStoreA::send_invalidate_direct` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1862 | `CxlKvStoreA::send_invalidate_direct` | `flush_line((void *)e);  // line 1 (req_op_id)` |
| `cxl_kv_ops_A.cc` | 1863 | `CxlKvStoreA::send_invalidate_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1902 | `CxlKvStoreA::enable_read_guard` | `flush_region(rcu, rcu_domain_bytes());` |
| `cxl_kv_ops_A.cc` | 1904 | `CxlKvStoreA::enable_read_guard` | `flush_region(haz, hazard_domain_bytes());` |
| `cxl_kv_ops_A.cc` | 1905 | `CxlKvStoreA::enable_read_guard` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1916 | `CxlKvStoreA::enable_invalidate` | `flush_region(ir, inval_ring_matrix_bytes());` |
| `cxl_kv_ops_A.cc` | 1917 | `CxlKvStoreA::enable_invalidate` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 1987 | `CxlKvStoreA::inval_receiver_loop` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 1988 | `CxlKvStoreA::inval_receiver_loop` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 1994 | `CxlKvStoreA::inval_receiver_loop` | `flush_line((void *)e);` |
| `cxl_kv_ops_A.cc` | 1995 | `CxlKvStoreA::inval_receiver_loop` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2006 | `CxlKvStoreA::inval_receiver_loop` | `flush_line((void *)e);` |
| `cxl_kv_ops_A.cc` | 2007 | `CxlKvStoreA::inval_receiver_loop` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2020 | `CxlKvStoreA::inval_receiver_loop` | `flush_line((void *)&e->resp_op_id);` |
| `cxl_kv_ops_A.cc` | 2021 | `CxlKvStoreA::inval_receiver_loop` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2066 | `CxlKvStoreA::forward_read_direct` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 2067 | `CxlKvStoreA::forward_read_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2076 | `CxlKvStoreA::forward_read_direct` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 2077 | `CxlKvStoreA::forward_read_direct` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2102 | `CxlKvStoreA::forward_read_direct` | `flush_line(&st->ready_op_id);` |
| `cxl_kv_ops_A.cc` | 2111 | `CxlKvStoreA::forward_read_direct` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 2112 | `CxlKvStoreA::forward_read_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2126 | `CxlKvStoreA::forward_read_direct` | `flush_line(&st->ready_op_id);` |
| `cxl_kv_ops_A.cc` | 2127 | `CxlKvStoreA::forward_read_direct` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2149 | `CxlKvStoreA::forward_read_direct` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 2150 | `CxlKvStoreA::forward_read_direct` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2239 | `CxlKvStoreA::write_handler` | `flush_line(&bucket->slots[0]);` |
| `cxl_kv_ops_A.cc` | 2240 | `CxlKvStoreA::write_handler` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2281 | `CxlKvStoreA::write_handler` | `flush_line((void *)(staging + off));` |
| `cxl_kv_ops_A.cc` | 2283 | `CxlKvStoreA::write_handler` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2319 | `CxlKvStoreA::read_handler` | `flush_line(st_noop);` |
| `cxl_kv_ops_A.cc` | 2320 | `CxlKvStoreA::read_handler` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2322 | `CxlKvStoreA::read_handler` | `flush_line(&st_noop->ready_op_id);` |
| `cxl_kv_ops_A.cc` | 2323 | `CxlKvStoreA::read_handler` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2330 | `CxlKvStoreA::read_handler` | `flush_line(bucket);` |
| `cxl_kv_ops_A.cc` | 2331 | `CxlKvStoreA::read_handler` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2338 | `CxlKvStoreA::read_handler` | `flush_line(st_noop);` |
| `cxl_kv_ops_A.cc` | 2339 | `CxlKvStoreA::read_handler` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2341 | `CxlKvStoreA::read_handler` | `flush_line(&st_noop->ready_op_id);` |
| `cxl_kv_ops_A.cc` | 2342 | `CxlKvStoreA::read_handler` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2349 | `CxlKvStoreA::read_handler` | `flush_line(bucket);` |
| `cxl_kv_ops_A.cc` | 2350 | `CxlKvStoreA::read_handler` | `flush_line((char *)bucket + 64);` |
| `cxl_kv_ops_A.cc` | 2351 | `CxlKvStoreA::read_handler` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2368 | `CxlKvStoreA::read_handler` | `flush_line(st);` |
| `cxl_kv_ops_A.cc` | 2369 | `CxlKvStoreA::read_handler` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2373 | `CxlKvStoreA::read_handler` | `flush_line(&st->ready_op_id);` |
| `cxl_kv_ops_A.cc` | 2374 | `CxlKvStoreA::read_handler` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2459 | `CxlKvStoreA::write_receiver_loop` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 2469 | `CxlKvStoreA::write_receiver_loop` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 2470 | `CxlKvStoreA::write_receiver_loop` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2477 | `CxlKvStoreA::write_receiver_loop` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 2500 | `CxlKvStoreA::write_receiver_loop` | `flush_line((void *)&e->resp_op_id);` |
| `cxl_kv_ops_A.cc` | 2501 | `CxlKvStoreA::write_receiver_loop` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2526 | `CxlKvStoreA::read_receiver_loop` | `flush_line((void *)&ring->tail);` |
| `cxl_kv_ops_A.cc` | 2527 | `CxlKvStoreA::read_receiver_loop` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2532 | `CxlKvStoreA::read_receiver_loop` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 2533 | `CxlKvStoreA::read_receiver_loop` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2545 | `CxlKvStoreA::read_receiver_loop` | `flush_line((void *)&e->req_op_id);` |
| `cxl_kv_ops_A.cc` | 2546 | `CxlKvStoreA::read_receiver_loop` | `full_fence();` |
| `cxl_kv_ops_A.cc` | 2568 | `CxlKvStoreA::read_receiver_loop` | `flush_line((void *)&e->resp_op_id);` |
| `cxl_kv_ops_A.cc` | 2569 | `CxlKvStoreA::read_receiver_loop` | `store_fence();` |
| `cxl_kv_ops_A.cc` | 2707 | `CxlKvStoreA::search` | `flush_line(bucket);` |
| `cxl_kv_ops_A.cc` | 2708 | `CxlKvStoreA::search` | `flush_line((char *)bucket + 64);` |
| `cxl_kv_ops_A.cc` | 2711 | `CxlKvStoreA::search` | `full_fence();` |
