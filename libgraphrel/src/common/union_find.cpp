/**
 * @file union_find.cpp
 * @brief Union-find with path compression and union by rank.
 */

#include "common/union_find.hpp"

#include <stdexcept>
#include <string>

namespace teddy::graphrel {

union_find::union_find(std::size_t n) {
  if (n > 65535) {
    throw std::invalid_argument(
        "union_find: maximum 65535 elements (got " + std::to_string(n) + ")");
  }
  parent_.resize(n);
  rank_.assign(n, 0);
  reset();
}

std::size_t union_find::find(std::size_t x) {
  if (x >= parent_.size()) {
    throw std::out_of_range("union_find::find: index out of range");
  }
  // Locate the root, then point every node on the path directly at it
  // (full path compression; iterative, so no recursion depth concerns).
  std::size_t root = x;
  while (parent_[root] != root) {
    root = parent_[root];
  }
  while (parent_[x] != root) {
    const std::size_t next = parent_[x];
    parent_[x] = static_cast<std::uint16_t>(root);
    x = next;
  }
  return root;
}

std::size_t union_find::find_root(std::size_t x) const {
  if (x >= parent_.size()) {
    throw std::out_of_range("union_find::find_root: index out of range");
  }
  std::size_t root = x;
  while (parent_[root] != root) {
    root = parent_[root];
  }
  return root;
}

void union_find::unite(std::size_t x, std::size_t y) {
  if (x >= parent_.size() || y >= parent_.size()) {
    throw std::out_of_range("union_find::unite: index out of range");
  }
  const std::size_t root_x = find(x);
  const std::size_t root_y = find(y);
  if (root_x == root_y) {
    return;
  }
  // Union by rank: attach the shallower tree below the deeper one.
  if (rank_[root_x] < rank_[root_y]) {
    parent_[root_x] = static_cast<std::uint16_t>(root_y);
  } else if (rank_[root_x] > rank_[root_y]) {
    parent_[root_y] = static_cast<std::uint16_t>(root_x);
  } else {
    parent_[root_y] = static_cast<std::uint16_t>(root_x);
    ++rank_[root_x];
  }
}

std::vector<std::vector<std::size_t>> union_find::get_classes() const {
  std::vector<std::vector<std::size_t>> by_root(parent_.size());
  for (std::size_t i = 0; i < parent_.size(); ++i) {
    by_root[find_root(i)].push_back(i);
  }
  std::vector<std::vector<std::size_t>> classes;
  for (auto &members : by_root) {
    if (!members.empty()) {
      classes.push_back(std::move(members));
    }
  }
  return classes;
}

void union_find::reset() {
  for (std::size_t i = 0; i < parent_.size(); ++i) {
    parent_[i] = static_cast<std::uint16_t>(i);
    rank_[i] = 0;
  }
}

} // namespace teddy::graphrel
