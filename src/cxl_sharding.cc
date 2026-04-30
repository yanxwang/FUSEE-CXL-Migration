#include "cxl_sharding.h"

#include <cstring>

namespace fusee {

int sharding_init(ShardingTable *st, uint32_t num_hosts) {
  if (!st) return -1;
  if (num_hosts == 0 || num_hosts > kMaxShardHosts) return -1;
  // power-of-two check
  if ((num_hosts & (num_hosts - 1)) != 0) return -1;

  std::memset(st, 0, sizeof(*st));
  st->num_hosts = num_hosts;
  st->mask = num_hosts - 1;

  // shift = 64 - log2(num_hosts), so the top log2(H) bits land in mask
  uint32_t log2h = 0;
  for (uint32_t v = num_hosts; v > 1; v >>= 1) log2h++;
  st->shift = 64u - log2h;

  return 0;
}

}  // namespace fusee
