#include "cxl_mm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

namespace fusee {

static inline size_t round_up(size_t n, size_t a) {
  return (n + a - 1) & ~(a - 1);
}

int cxl_region_init(CXLRegion *out, const char *dev_path,
                    size_t requested_size) {
  if (!out || !dev_path || requested_size == 0) {
    errno = EINVAL;
    return -1;
  }

  memset(out, 0, sizeof(*out));
  snprintf(out->dev_path, sizeof(out->dev_path), "%s", dev_path);

  size_t size = round_up(requested_size, kCxlDevdaxAlign);

  // O_CREAT is harmless for /dev/dax* (already exists) and lets us use regular
  // files (tmpfs / scratch) interchangeably during development.
  int fd = open(dev_path, O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    perror("cxl_region_init: open");
    return -1;
  }

  // ftruncate fails with EINVAL on char devdax; ignore that, fail on others.
  if (ftruncate(fd, size) < 0 && errno != EINVAL) {
    perror("cxl_region_init: ftruncate");
    close(fd);
    return -1;
  }

  void *base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    perror("cxl_region_init: mmap");
    close(fd);
    return -1;
  }

  out->fd = fd;
  out->base = base;
  out->size = size;
  return 0;
}

void cxl_region_destroy(CXLRegion *r) {
  if (!r) return;
  if (r->base && r->base != MAP_FAILED) {
    munmap(r->base, r->size);
  }
  if (r->fd > 0) {
    close(r->fd);
  }
  memset(r, 0, sizeof(*r));
}

} // namespace fusee
