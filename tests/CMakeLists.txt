if(NOT CXL_ONLY)
    find_package(GTest REQUIRED)

    include_directories(${GTEST_INCLUDE_DIRS})

    add_executable(test_nm test_nm.cc ddckv_test.cc)
    add_executable(test_kv_utils test_kv_utils.cc ddckv_test.cc)
    # add_executable(test_server test_server.cc ddckv_test.cc)
    add_executable(test_client_server test_client_server.cc)
    add_executable(test_client_client test_client_client.cc)
    add_executable(test_remote_nm test_remote_nm.cc ddckv_test.cc)
    add_executable(client_kv_shell client_kv_shell.cc)
    add_executable(test_mm test_mm.cc ddckv_test.cc)
endif()

# CXL migration tests: all link against libfusee_cxl (which bundles cxl_mm,
# cxl_bucket_lock, cxl_kv_ops_C, and the LFM mutex), so no per-test source
# lists are needed.
foreach(t cxl_mm_test cxl_mm_mp_test cxl_bucket_lock_test
         cxl_kv_ops_C_test cxl_kv_ops_B_test
         cxl_oplog_test cxl_kv_ops_C_oplog_test cxl_oplog_redo_test
         cxl_kv_ops_C_recover_redo_test
         cxl_kv_ops_B_recover_redo_test
         cxl_dram_cache_test cxl_dram_cache_B_test
         cxl_xhost_test cxl_latency_decomp
         cxl_kv_bench_threads cxl_atomic_bench cxl_atomic_xhost
         cxl_blockpool_test cxl_dualhost_bw_bench cxl_a_aggr_test
         protocol_a_ycsb protocol_a_rw_race_test
         protocol_a_invariant_check
         cxl_primitive_bench
         cxl_write_atomicity_probe
         cxl_false_sharing_probe
         cxl_concurrent_reader_probe)
    add_executable(${t} ${t}.cc)
    target_link_libraries(${t} fusee_cxl)
    target_compile_options(${t} PRIVATE -mclflushopt -mavx512f)
endforeach()

# iter-4A pure-CPU unit tests (no AVX-512 dependency, runnable on workstation).
foreach(t sharding_test directory_test cache_pool_test blockpool_freelist_test per_host_msg_test protocol_a_local_test)
    add_executable(${t} ${t}.cc)
    target_link_libraries(${t} fusee_cxl)
    target_compile_options(${t} PRIVATE -O2 -g)
endforeach()

# Protocol F (FUSEE-on-CXL) unit tests — see docs/protocol_F_design_and_plan.md.
# Tests that touch the CXL flush primitives (pool) are compiled with
# -mclflushopt; pure-DRAM tests (index cache) are not, so they run on the
# workstation Broadwell-EP CPU without SIGILL.
foreach(t cxl_fusee_kvpair_pool_test cxl_kv_ops_F_test
         cxl_kv_ops_F_concurrent_test cxl_kv_ops_F_2host_hashdiff
         cxl_kv_ops_F_microbench cxl_kv_ops_F_micro_throughput
         cxl_kv_ops_F_ycsb)
    add_executable(${t} ${t}.cc)
    target_link_libraries(${t} fusee_cxl)
    target_compile_options(${t} PRIVATE -mclflushopt -O2 -g)
endforeach()
foreach(t cxl_fusee_index_cache_test)
    add_executable(${t} ${t}.cc)
    target_link_libraries(${t} fusee_cxl)
    target_compile_options(${t} PRIVATE -O2 -g)
endforeach()

# Per-stage C write-path latency decomposition runner. Links the
# decomp-instrumented sibling library so the insert/update stage timestamps
# in cxl_kv_ops_C.cc are live. Uses CONSENSUS_OPT=3 (protocol C).
add_executable(cxl_latency_decomp_C cxl_latency_decomp_C.cc)
target_link_libraries(cxl_latency_decomp_C fusee_cxl_decomp)
target_compile_options(cxl_latency_decomp_C
    PRIVATE -mclflushopt -O2 -DCONSENSUS_OPT=3)

# iter-9A note: cxl_latency_decomp_A (iter-1A's per-stage A write-path
# decomposition runner) was superseded by `protocol_a_ycsb` +
# `src/cxl_probe.h` instrumentation when A switched to v2 in iter-4A.
# The original source still uses iter-2A LRC APIs (enable_dram_cache,
# enable_per_host_ring) that are gone. iter-9A path_decomp Phase 3 runs
# against protocol_a_ycsb directly, so this build target is dropped.

# Unified micro-bench + YCSB runner: one binary per (protocol × variant).
#
# iter-9A note: the A variants of cxl_{kv_bench,kv_bench_mp,ycsb_runner}
# call iter-2A LRC APIs (enable_dram_cache, enable_same_host_bypass,
# enable_per_host_ring, replicated_ops, ack_timeouts_to) that were
# removed when Protocol A switched to the v2 directory architecture in
# iter-4A. These legacy A variants have been silently broken in the
# build since then. The canonical Protocol A test runner is
# `protocol_a_ycsb` (iter-4A onward); A's micro-bench is
# `cxl_a_aggr_test`. iter-9A keeps B/C variants of the legacy templates
# (still wired against current B/C surface) and skips A.
foreach(opt B C F)
    if(${opt} STREQUAL "B")
        set(opt_num 2)
    elseif(${opt} STREQUAL "C")
        set(opt_num 3)
    else()
        set(opt_num 4)
    endif()
    foreach(variant kv_bench kv_bench_mp ycsb_runner)
        add_executable(cxl_${variant}_${opt} cxl_${variant}.cc)
        target_link_libraries(cxl_${variant}_${opt} fusee_cxl)
        target_compile_options(cxl_${variant}_${opt}
            PRIVATE -mclflushopt -O2
            -DCONSENSUS_OPT=${opt_num})
    endforeach()
endforeach()

if(NOT CXL_ONLY)
    target_link_libraries(test_remote_nm
        ${GTEST_BOTH_LIBRARIES}
        libddckv
        pthread
        ibverbs
    )

    target_link_libraries(test_kv_utils
        ${GTEST_BOTH_LIBRARIES}
        libddckv
        pthread
    )

    target_link_libraries(test_nm
        ${GTEST_BOTH_LIBRARIES}
        libddckv
        pthread
        ibverbs
    )

    target_link_libraries(test_client_server
        libddckv
        pthread
        ibverbs
    )

    target_compile_options(
        test_client_client
        PRIVATE
    )

    target_link_libraries(test_client_client
        ${GTEST_BOTH_LIBRARIES}
        libddckv
        pthread
        ibverbs
    )

    target_link_libraries(client_kv_shell
        libddckv
        pthread
        ibverbs
    )

    target_link_libraries(test_mm
        ${GTEST_BOTH_LIBRARIES}
        libddckv
        pthread
        ibverbs
    )

    gtest_discover_tests(
        test_nm
        test_remote_nm
        test_kv_utils
        # test_server
        test_client_client
        test_mm
    )
endif()