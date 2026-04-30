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
    cxl_oplog.cc
    cxl_batch_ring.cc
    cxl_kv_blockpool.cc
    cxl_a_local_aggregator.cc
    cxl_latency_decomp_probe.cc
    cxl_sharding.cc
    cxl_directory.cc
    ${CXL_SHM_PROFILING_DIR}/locks/lfm_lock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ticket_lock_fusee_patched.c)

add_library(fusee_cxl STATIC ${source_fusee_cxl})
target_include_directories(fusee_cxl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CXL_SHM_PROFILING_DIR})
target_compile_options(fusee_cxl PRIVATE -O2 -g -mclflushopt)
# Phase 2: opt-in ticket lock. Build with -DFUSEE_USE_TICKET_LOCK=ON to swap
# LFM → ticket lock in BucketLockEntry. Default OFF (keep LFM) to make the
# phase comparable to the baseline scaling sweep.
option(FUSEE_USE_TICKET_LOCK "Use ticket_lock instead of LFM in BucketLockEntry" OFF)
if(FUSEE_USE_TICKET_LOCK)
    target_compile_definitions(fusee_cxl PUBLIC FUSEE_USE_TICKET_LOCK=1)
endif()
# Phase 2b: per-slot lock for protocol C. When ON, BucketLockEntry is
# replaced by SlotLockEntry holding 7 ticket_mutex_t (one per slot). The
# C write path acquires only the target slot's mutex; readers keep the
# per-bucket write_epoch seqlock.
option(FUSEE_PER_SLOT_LOCK "Use per-slot ticket lock for protocol C writes" OFF)
if(FUSEE_PER_SLOT_LOCK)
    target_compile_definitions(fusee_cxl PUBLIC FUSEE_PER_SLOT_LOCK=1)
endif()
# Phase-3 micro-batching: drain merges same-slot writes to last-value-wins.
option(FUSEE_BATCH_MERGE_SAME_KEY "Coalesce same-slot writes during flusher drain" ON)
if(FUSEE_BATCH_MERGE_SAME_KEY)
    target_compile_definitions(fusee_cxl PUBLIC FUSEE_BATCH_MERGE_SAME_KEY=1)
endif()
# 2f: ring-depth sweep knob. -DFUSEE_RING_ENTRIES=N at CMake config time
# sizes the per-(src,dst) PendingRing. Default 256 (see cxl_pending_ring.h).
if(DEFINED FUSEE_RING_ENTRIES)
    target_compile_definitions(fusee_cxl PUBLIC FUSEE_RING_ENTRIES=${FUSEE_RING_ENTRIES})
endif()
target_link_libraries(fusee_cxl PUBLIC pthread)

# Decomp-instrumented sibling of fusee_cxl for cxl_latency_decomp_C. Same
# sources, compiled with FUSEE_LATENCY_DECOMP=1 so the insert/update stage
# timestamps in cxl_kv_ops_C.cc are live. Production binaries link
# fusee_cxl (no instrumentation → zero runtime cost).
# Phase-1 anatomy: if FUSEE_LFM_INSTRUMENT=ON, also swap the upstream
# lfm_lock.c for src/lfm_lock_fusee_instrumented.c which publishes
# 4 per-LFM-stage samples into the DecompProbe on every acquire.
option(FUSEE_LFM_INSTRUMENT "Link instrumented LFM into fusee_cxl_decomp" OFF)
set(source_fusee_cxl_decomp ${source_fusee_cxl})
if(FUSEE_LFM_INSTRUMENT)
    list(REMOVE_ITEM source_fusee_cxl_decomp
        "${CXL_SHM_PROFILING_DIR}/locks/lfm_lock.c")
    list(APPEND source_fusee_cxl_decomp
        "${CMAKE_CURRENT_SOURCE_DIR}/lfm_lock_fusee_instrumented.c")
endif()
add_library(fusee_cxl_decomp STATIC ${source_fusee_cxl_decomp})
target_include_directories(fusee_cxl_decomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CXL_SHM_PROFILING_DIR})
target_compile_options(fusee_cxl_decomp PRIVATE -O2 -g -mclflushopt)
target_compile_definitions(fusee_cxl_decomp PUBLIC FUSEE_LATENCY_DECOMP=1)
if(FUSEE_USE_TICKET_LOCK)
    target_compile_definitions(fusee_cxl_decomp PUBLIC FUSEE_USE_TICKET_LOCK=1)
endif()
if(FUSEE_PER_SLOT_LOCK)
    target_compile_definitions(fusee_cxl_decomp PUBLIC FUSEE_PER_SLOT_LOCK=1)
endif()
if(FUSEE_BATCH_MERGE_SAME_KEY)
    target_compile_definitions(fusee_cxl_decomp PUBLIC FUSEE_BATCH_MERGE_SAME_KEY=1)
endif()
if(FUSEE_LFM_INSTRUMENT)
    target_compile_definitions(fusee_cxl_decomp PUBLIC FUSEE_LFM_INSTRUMENT=1)
endif()
if(DEFINED FUSEE_RING_ENTRIES)
    target_compile_definitions(fusee_cxl_decomp PUBLIC FUSEE_RING_ENTRIES=${FUSEE_RING_ENTRIES})
endif()
target_link_libraries(fusee_cxl_decomp PUBLIC pthread)

