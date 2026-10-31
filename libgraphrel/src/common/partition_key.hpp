#ifndef GRAPHREL_SRC_PARTITION_KEY_HPP
#define GRAPHREL_SRC_PARTITION_KEY_HPP

#include "common/partition.hpp"
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>

namespace teddy::graphrel {

/**
 * @struct partition_key
 * @brief Optimized key for hash map lookup (192-bit, cache-friendly)
 *
 * Replaces vector-based PartitionKey with fixed-size struct for LBL algorithm:
 * - partition_number: Stirling encoding of partition structure
 * - k_bitset: Bitmask of which blocks contain K-vertices
 * - boundary_hash: XXH3-style hash of absolute boundary vertex IDs
 *
 * Design rationale:
 * - 24 bytes (3x uint64_t) fits in 1.5 cache lines
 * - No dynamic allocations (vs. std::vector in PartitionKey)
 * - boundary_hash provides absolute vertex identity (solves N > 64 limitation)
 * - Supports unlimited network size via hashing
 */
struct alignas(16) partition_key {
  /// Boundary sets are limited by 64-bit partition numbers (see
  /// bell_cache::max_supported_n); k_bitset therefore never needs > 25 bits.
  static constexpr std::size_t MAX_BOUNDARY_SIZE = bell_cache::max_supported_n;

  std::uint64_t partition_number; // Stirling-encoded partition structure
  std::uint64_t k_bitset;      // Bitmask: bit i = 1 if block i has K-vertices
  std::uint64_t boundary_hash; // Hash of sorted boundary vertex IDs

  /**
   * @brief Equality operator for hash table
   */
  bool operator==(const partition_key &other) const {
    return partition_number == other.partition_number &&
           k_bitset == other.k_bitset && boundary_hash == other.boundary_hash;
  }

  /**
   * @brief Inequality operator
   */
  bool operator!=(const partition_key &other) const {
    return !(*this == other);
  }
};

/**
 * @brief Convert partition_key to human-readable string for debugging
 *
 * Note: partition_number and boundary_hash are opaque. Use StirlingBellCache
 * to decode partition_number if needed.
 */
inline std::string to_string(const partition_key &key) {
  std::ostringstream oss;
  oss << "partition_key{";
  oss << "partition=0x" << std::hex << key.partition_number << std::dec;
  oss << ", k_bitset=0b" << std::bitset<16>(key.k_bitset);
  oss << ", boundary_hash=0x" << std::hex << key.boundary_hash << std::dec;
  oss << "}";
  return oss.str();
}

inline std::ostream &operator<<(std::ostream &os,
                                const partition_key &key) {
  os << to_string(key);
  return os;
}

} // namespace teddy::graphrel

// Hash function for partition_key
namespace std {
/**
 * @brief Hash function for partition_key
 *
 * Uses mixed constants to reduce correlation between fields.
 */
template <> struct hash<teddy::graphrel::partition_key> {
  std::size_t
  operator()(const teddy::graphrel::partition_key &k) const {
    // Combine three fields with different constants (reduces correlation)
    std::uint64_t h = k.partition_number;
    h ^= k.k_bitset + 0x9E3779B97F4A7C15ULL;      // Golden ratio
    h ^= k.boundary_hash + 0x517CC1B727220A95ULL; // Different constant

    // Final mix (WyHash style)
    h *= 0x9E3779B97F4A7C15ULL;
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;

    return static_cast<std::size_t>(h);
  }
};
} // namespace std

#endif // GRAPHREL_SRC_PARTITION_KEY_HPP
