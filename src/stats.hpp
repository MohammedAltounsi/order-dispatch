#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// Percentile by nearest rank on a sorted copy. Averages hide the slow tail; p99 shows it.
inline std::int64_t percentile(std::vector<std::int64_t> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  std::size_t rank = static_cast<std::size_t>(p / 100.0 * (v.size() - 1) + 0.5);
  return v[std::min(rank, v.size() - 1)];
}
