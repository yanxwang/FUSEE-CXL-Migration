if(NOT CXL_ONLY)
    # libddckv remains the original RDMA-era build. CXL sources live in
    # libfusee_cxl (below) so libddckv does not need the cxl_shm_profiling
    # include path or LFM dependency.
    list(APPEND source_ddckv
        nm.cc
        ib.cc
        server_mm.cc
        server.cc
        kv_utils.cc
        hashtable.cc
        client_mm.cc
        client.cc
        client_cr.cc)

    add_library(libddckv STATIC ${source_ddckv})

    target_compile_options(
        libddckv
        PRIVATE
        "-O2"
        # ${CMAKE_CXX_FLAGS_DEBUG}
        "-g"
        # "-D_DEBUG"
    )

    target_link_libraries(libddckv ${Boost_LIBRARIES} boost_context boost_fiber tbb)
endif()

# -----------------------------------------------------------------------------
# libfusee_cxl: RDMA-free static library for the CXL migration path.
# Contains only the new cxl_*.cc modules plus the LFM mutex from
# cxl_shm_profiling. Tests can link this to avoid pulling in libddckv (which
# still carries RDMA deps until the full Phase-4 RDMA removal lands).
# -----------------------------------------------------------------------------
set(source_fusee_cxl
    cxl_mm.cc
    cxl_bucket_lock.cc
    cxl_kv_ops_C.cc
    cxl_kv_ops_A.cc
    cxl_kv_ops_B.cc
    ${CXL_SHM_PROFILING_DIR}/locks/lfm_lock.c)

add_library(fusee_cxl STATIC ${source_fusee_cxl})
target_include_directories(fusee_cxl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CXL_SHM_PROFILING_DIR})
target_compile_options(fusee_cxl PRIVATE -O2 -g -mclflushopt)
target_link_libraries(fusee_cxl PUBLIC pthread)

