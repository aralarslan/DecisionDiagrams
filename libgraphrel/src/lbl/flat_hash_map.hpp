#ifndef GRAPHREL_SRC_LBL_FLAT_HASH_MAP_HPP
#define GRAPHREL_SRC_LBL_FLAT_HASH_MAP_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

/**
 * @file flat_hash_map.hpp
 * @brief Open-addressing hash table optimized for Wu & Sun Layer-by-Layer
 * algorithm
 *
 * Key features:
 * - Linear probing for cache-friendly lookups
 * - Optimized clear() that preserves capacity (O(capacity) or better)
 * - Ping-pong buffering support (swap without deallocation)
 * - Fixed load factor management (0.75 default)
 */

namespace teddy::graphrel {

/**
 * @class flat_hash_map
 * @brief Open-addressing hash table with linear probing
 *
 * Design goals:
 * - Cache-friendly: All data in contiguous arrays
 * - Fast clear: Reset without deallocation
 * - Predictable performance: Fixed load factor, power-of-2 sizing
 *
 * Memory layout:
 * - entries_: Contiguous array of {key, value} pairs
 * - metadata_: Parallel array tracking slot state (empty/occupied/deleted)
 *
 * @tparam Key Key type (must be hashable and equality-comparable)
 * @tparam Value Value type
 * @tparam Hash Hash function type (default: std::hash<Key>)
 */
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class flat_hash_map {
public:
  // Forward declarations
  class iterator;

  /**
   * @brief Entry stored in the hash table
   */
  struct Entry {
    Key key{};     // Default-initialize key
    Value value{}; // Default-initialize value
  };

  /**
   * @brief Metadata for each slot
   */
  enum class SlotState : std::uint8_t {
    EMPTY = 0,   // Slot never used
    OCCUPIED = 1 // Slot contains valid entry (no erase, so no tombstones)
  };

private:
  std::vector<Entry> entries_;      // Key-value storage
  std::vector<SlotState> metadata_; // Slot state tracking
  std::size_t size_;                // Number of occupied slots
  std::size_t capacity_;            // Total slots (power of 2)
  float max_load_factor_;           // Threshold for resize (default 0.75)
  Hash hasher_;                     // Hash function
  mutable std::size_t max_probe_length_ = 0; // Max probe sequence length
  std::size_t resize_count_ = 0;             // Number of resizes performed

  /**
   * @brief Compute index from hash (uses bitmask for power-of-2 capacity)
   */
  std::size_t index_from_hash(std::size_t hash) const {
    // capacity_ is always power of 2, so (capacity_ - 1) is a perfect mask
    return hash & (capacity_ - 1);
  }

  /**
   * @brief Find the slot for a given key (for insertion or lookup)
   * @return {slot_index, found}
   */
  std::pair<std::size_t, bool> find_slot(const Key &key) const {
    if (capacity_ == 0) {
      return {0, false};
    }

    std::size_t hash = hasher_(key);
    std::size_t idx = index_from_hash(hash);
    std::size_t start_idx = idx;

    // Linear probing
    std::size_t probes = 0;
    do {
      probes++;
      if (metadata_[idx] == SlotState::EMPTY) {
        // Found empty slot (key not present)
        if (probes > max_probe_length_) {
          max_probe_length_ = probes;
        }
        return {idx, false};
      }
      if (metadata_[idx] == SlotState::OCCUPIED) {
        if (entries_[idx].key == key) {
          // Found matching key
          if (probes > max_probe_length_) {
            max_probe_length_ = probes;
          }
          return {idx, true};
        }
      }
      // Continue probing (wrap around using mask)
      idx = (idx + 1) & (capacity_ - 1);
    } while (idx != start_idx);

    // Table is full (should never happen with proper load factor)
    assert(false && "Hash table full - resize should have been triggered");
    return {0, false};
  }

  /**
   * @brief Resize the hash table to new capacity (must be power of 2)
   */
  void resize(std::size_t new_capacity) {
    assert((new_capacity & (new_capacity - 1)) == 0 &&
           "Capacity must be power of 2");

    ++resize_count_;

    // Save old data
    std::vector<Entry> old_entries = std::move(entries_);
    std::vector<SlotState> old_metadata = std::move(metadata_);
    std::size_t old_capacity = capacity_;

    // Allocate new storage
    capacity_ = new_capacity;
    entries_.resize(capacity_);
    metadata_.resize(capacity_, SlotState::EMPTY);
    size_ = 0;

    // Rehash all entries
    for (std::size_t i = 0; i < old_capacity; ++i) {
      if (old_metadata[i] == SlotState::OCCUPIED) {
        // Re-insert into new table
        insert_internal(std::move(old_entries[i].key),
                        std::move(old_entries[i].value));
      }
    }
  }

  /**
   * @brief Internal insertion (assumes capacity check already done)
   */
  void insert_internal(Key &&key, Value &&value) {
    std::size_t hash = hasher_(key);
    std::size_t idx = index_from_hash(hash);

    // Linear probing to find empty slot
    while (metadata_[idx] == SlotState::OCCUPIED) {
      idx = (idx + 1) & (capacity_ - 1);
    }

    // Insert at empty slot
    entries_[idx].key = std::move(key);
    entries_[idx].value = std::move(value);
    metadata_[idx] = SlotState::OCCUPIED;
    ++size_;
  }

  /**
   * @brief Check if resize is needed and perform it
   */
  void maybe_resize() {
    if (capacity_ == 0 || static_cast<double>(size_ + 1) >
                              static_cast<double>(max_load_factor_) *
                                  static_cast<double>(capacity_)) {
      std::size_t new_capacity = (capacity_ == 0) ? 16 : capacity_ * 2;
      resize(new_capacity);
    }
  }

public:
  /**
   * @brief Constructor with initial capacity
   * @param initial_capacity Initial capacity (will be rounded up to power of 2)
   * @param max_load_factor Maximum load factor before resize (default 0.75)
   */
  explicit flat_hash_map(std::size_t initial_capacity = 0,
                       float max_load_factor = 0.75f)
      : size_(0), capacity_(0), max_load_factor_(max_load_factor) {
    if (initial_capacity > 0) {
      // Round up to nearest power of 2
      std::size_t cap = 1;
      while (cap < initial_capacity) {
        cap *= 2;
      }
      capacity_ = cap;
      entries_.resize(capacity_);
      metadata_.resize(capacity_, SlotState::EMPTY);
    }
  }

  /**
   * @brief Get current size (number of entries)
   */
  std::size_t size() const { return size_; }

  /**
   * @brief Get current capacity
   */
  std::size_t capacity() const { return capacity_; }

  /**
   * @brief Check if map is empty
   */
  bool empty() const { return size_ == 0; }

  /**
   * @brief Get current load factor
   */
  float load_factor() const {
    return capacity_ > 0 ? static_cast<float>(static_cast<double>(size_) /
                                              static_cast<double>(capacity_))
                         : 0.0f;
  }

  /**
   * @brief Get maximum probe length observed
   */
  std::size_t get_max_probe_length() const { return max_probe_length_; }

  /**
   * @brief Get total number of resizes performed
   */
  std::size_t get_resize_count() const { return resize_count_; }


  /**
   * @brief Emplace (insert) key-value pair
   *
   * If key exists, does NOT replace value. Otherwise inserts new entry.
   *
   * @param key Key to insert
   * @param value Value to insert
   * @return Pair of {iterator, inserted} where inserted is true if new
   * insertion
   */
  std::pair<iterator, bool> emplace(const Key &key, const Value &value) {
    maybe_resize();

    auto [idx, found] = find_slot(key);

    if (found) {
      // Key exists, do NOT replace (emplace never replaces)
      return {iterator(this, idx), false};
    }

    // Insert new entry
    entries_[idx].key = key;
    entries_[idx].value = value;
    metadata_[idx] = SlotState::OCCUPIED;
    ++size_;

    return {iterator(this, idx), true};
  }

  /**
   * @brief Swap with another map (for ping-pong buffering)
   */
  void swap(flat_hash_map &other) noexcept {
    std::swap(entries_, other.entries_);
    std::swap(metadata_, other.metadata_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(max_load_factor_, other.max_load_factor_);
    std::swap(max_probe_length_, other.max_probe_length_);
    std::swap(resize_count_, other.resize_count_);
  }

  /**
   * @brief Clear the map (optimized for ping-pong buffering)
   *
   * CRITICAL: Does NOT deallocate memory. Only resets metadata.
   * This makes clear() very fast - O(capacity) or better.
   * Perfect for ping-pong buffering where we swap maps between layers.
   */
  void clear() {
    // Reset size
    size_ = 0;

    // Reset metadata (mark all slots as empty)
    // This is the ONLY operation needed for correctness
    std::fill(metadata_.begin(), metadata_.end(), SlotState::EMPTY);

    // NOTE: We do NOT clear entries_ vector or deallocate memory
    // This preserves capacity for reuse (critical for ping-pong buffering)
  }

  /**
   * @brief Find a key and return pointer to value (nullptr if not found)
   */
  const Value *find(const Key &key) const {
    auto [idx, found] = find_slot(key);
    return found ? &entries_[idx].value : nullptr;
  }

  Value *find(const Key &key) {
    auto [idx, found] = find_slot(key);
    return found ? &entries_[idx].value : nullptr;
  }

  /**
   * @brief Check if key exists
   */
  bool contains(const Key &key) const { return find_slot(key).second; }

  /**
   * @brief Reserve capacity (rounds up to power of 2)
   */
  void reserve(std::size_t min_capacity) {
    if (min_capacity <= capacity_)
      return;

    std::size_t new_capacity = 1;
    while (new_capacity < min_capacity) {
      new_capacity *= 2;
    }

    if (new_capacity > capacity_) {
      resize(new_capacity);
    }
  }

  /**
   * @brief Iterator support for range-based for loops
   */
  class iterator {
    flat_hash_map *map_;
    std::size_t idx_;

    void advance() {
      while (idx_ < map_->capacity_ &&
             map_->metadata_[idx_] != SlotState::OCCUPIED) {
        ++idx_;
      }
    }

  public:
    iterator(flat_hash_map *map, std::size_t idx) : map_(map), idx_(idx) {
      advance();
    }

    Entry &operator*() { return map_->entries_[idx_]; }
    Entry *operator->() { return &map_->entries_[idx_]; }

    iterator &operator++() {
      ++idx_;
      advance();
      return *this;
    }

    bool operator!=(const iterator &other) const { return idx_ != other.idx_; }
  };

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, capacity_); }
};

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_LBL_FLAT_HASH_MAP_HPP
