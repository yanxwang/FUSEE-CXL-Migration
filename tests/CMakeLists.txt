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
         cxl_kv_ops_C_test cxl_kv_ops_A_test cxl_kv_ops_B_test
         cxl_oplog_test cxl_kv_ops_C_oplog_test cxl_oplog_redo_test
         cxl_kv_ops_C_recover_redo_test
         cxl_kv_ops_A_recover_redo_test cxl_kv_ops_B_recover_redo_test
         cxl_dram_cache_test cxl_dram_cache_B_test
         cxl_xhost_test cxl_latency_decomp
         cxl_kv_bench_threads cxl_atomic_bench cxl_atomic_xhost
         cxl_blockpool_test cxl_dualhost_bw_bench cxl_a_aggr_test)
    add_executable(${t} ${t}.cc)
    target_link_libraries(${t} fusee_cxl)
    target_compile_options(${t} PRIVATE -mclflushopt -mavx512f)
endforeach()

# iter-4A pure-CPU unit tests (no AVX-512 dependency, runnable on workstation).
foreach(t sharding_test)
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

# iter-1A: per-stage A write-path latency decomposition runner. Same
# harness shape as cxl_latency_decomp_C, built against CxlKvStoreA via
# CONSENSUS_OPT=1. The DECOMP_REC calls live in src/cxl_kv_ops_A.cc
# (added in iter-1A); the kDecompStage* enum slots are re-used with
# A-specific semantics documented in
# docs/iters/latency_decomp_A_iter1_20260426.md.
add_executable(cxl_latency_decomp_A cxl_latency_decomp_A.cc)
target_link_libraries(cxl_latency_decomp_A fusee_cxl_decomp)
target_compile_options(cxl_latency_decomp_A
    PRIVATE -mclflushopt -O2 -DCONSENSUS_OPT=1)

# Unified micro-bench + YCSB runner: one binary per (protocol × variant).
foreach(opt A B C)
    if(${opt} STREQUAL "A")
        set(opt_num 1)
    elseif(${opt} STREQUAL "B")
        set(opt_num 2)
    else()
        set(opt_num 3)
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