#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Nearest-rank percentile on an already sorted vector: the smallest sample with
// at least p% of all samples at or below it. Sort once, then ask for p50/p99/p99.9.
inline std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  if (p <= 0) return sorted.front();
  auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
  return sorted[std::min(rank, sorted.size()) - 1];
}
