cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(DDCKV LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 11)

find_package(Boost REQUIRED)

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

add_subdirectory(ycsb-test)
add_subdirectory(crash-recover-test)
add_subdirectory(micro-test)

enable_testing()
add_subdirectory(tests)