#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Nearest-rank percentile. Expects a sorted vector, so sort once and call it a few times.
inline std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  if (p <= 0) return sorted.front();
  auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
  return sorted[std::min(rank, sorted.size()) - 1];
}
