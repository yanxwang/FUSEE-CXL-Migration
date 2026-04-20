cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(DDCKV LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 11)

# CXL_ONLY=ON skips everything that depends on RDMA (libddckv's RDMA sources,
# ycsb-test, crash-recover-test, micro-test, and the RDMA unit tests). The
# resulting build produces only libfusee_cxl + the CXL migration tests. This
# is the "Phase 4" intermediate — full deletion of RDMA code waits until we
# decide we never want the RDMA path back.
option(CXL_ONLY "Build only the CXL migration path; skip RDMA sources" OFF)

if(NOT CXL_ONLY)
    find_package(Boost REQUIRED)
endif()

include_directories(src)
set(CMAKE_BUILD_TYPE Release)

# Path to the cxl_shm_profiling repo (provides LFM mutex, CXL primitives).
# Override with -DCXL_SHM_PROFILING_DIR=/path/to/cxl_shm_profiling at
# configure time if it lives elsewhere.
if(NOT DEFINED CXL_SHM_PROFILING_DIR)
    set(CXL_SHM_PROFILING_DIR "$ENV{HOME}/cxl_shm_profiling"
        CACHE PATH "Path to cxl_shm_profiling checkout")
endif()
if(NOT EXISTS "${CXL_SHM_PROFILING_DIR}/shm_mutex.h")
    message(WARNING "cxl_shm_profiling not found at ${CXL_SHM_PROFILING_DIR};"
        " CXL bucket-lock targets will fail to build until -DCXL_SHM_PROFILING_DIR is set.")
endif()

add_subdirectory(src)

if(NOT CXL_ONLY)
    add_subdirectory(ycsb-test)
    add_subdirectory(crash-recover-test)
    add_subdirectory(micro-test)
endif()

enable_testing()
add_subdirectory(tests)