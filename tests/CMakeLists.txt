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

# Phase 1.1: CXL mm standalone sanity test (no RDMA deps, no libddckv).
add_executable(cxl_mm_test cxl_mm_test.cc ../src/cxl_mm.cc)
target_include_directories(cxl_mm_test PRIVATE ../src)

# Phase 1.2: multi-process shared-region verification.
add_executable(cxl_mm_mp_test cxl_mm_mp_test.cc ../src/cxl_mm.cc)
target_include_directories(cxl_mm_mp_test PRIVATE ../src)
target_compile_options(cxl_mm_mp_test PRIVATE -mclflushopt)

# Phase 2: CXL bucket-lock (LFM-backed) multi-process contention test.
# Compiles in cxl_shm_profiling's LFM implementation directly so the test
# binary is self-contained and does not depend on libddckv (which still has
# RDMA deps until Phase 4).
add_executable(cxl_bucket_lock_test
    cxl_bucket_lock_test.cc
    ../src/cxl_mm.cc
    ../src/cxl_bucket_lock.cc
    ${CXL_SHM_PROFILING_DIR}/locks/lfm_lock.c)
target_include_directories(cxl_bucket_lock_test PRIVATE
    ../src
    ${CXL_SHM_PROFILING_DIR})
target_compile_options(cxl_bucket_lock_test PRIVATE -mclflushopt)

# Phase 3: Option C KV ops correctness test (two processes, cross-host ops).
add_executable(cxl_kv_ops_C_test
    cxl_kv_ops_C_test.cc
    ../src/cxl_mm.cc
    ../src/cxl_bucket_lock.cc
    ../src/cxl_kv_ops_C.cc
    ${CXL_SHM_PROFILING_DIR}/locks/lfm_lock.c)
target_include_directories(cxl_kv_ops_C_test PRIVATE
    ../src
    ${CXL_SHM_PROFILING_DIR})
target_compile_options(cxl_kv_ops_C_test PRIVATE -mclflushopt)

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

# target_link_libraries(test_server
#     ${GTEST_BOTH_LIBRARIES}
#     libddckv
#     pthread
#     ibverbs
# )

target_link_libraries(test_client_server 
    libddckv
    pthread
    ibverbs
)

target_compile_options(
    test_client_client
    PRIVATE
    # ${CMAKE_CXX_FLAGS_DEBUG}
    # "-g"
    # "-D_DEBUG"
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