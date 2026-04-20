#ifndef FUSEE_CXL_MM_H_
#define FUSEE_CXL_MM_H_

#include <stddef.h>
#include <stdint.h>

namespace fusee {

// devdax 2 MB page alignment requirement
constexpr size_t kCxlDevdaxAlign = 2UL * 1024 * 1024;

struct CXLRegion {
  int fd;            // file descriptor for /dev/dax*.*  or backing file
  void *base;        // mmap'd base (page-aligned)
  size_t size;       // rounded-up mapped size
  char dev_path[64]; // source device/file path
};

// Open `dev_path` (e.g. "/dev/dax0.0" or a regular file on tmpfs),
// ftruncate if the backing supports it, mmap PROT_READ|PROT_WRITE MAP_SHARED.
// `requested_size` is rounded up to kCxlDevdaxAlign.
// Returns 0 on success, -1 on failure (errno set).
int cxl_region_init(CXLRegion *out, const char *dev_path,
                    size_t requested_size);

// munmap + close. Safe to call on a zero-initialized CXLRegion.
void cxl_region_destroy(CXLRegion *r);

} // namespace fusee

#endif // FUSEE_CXL_MM_H_
