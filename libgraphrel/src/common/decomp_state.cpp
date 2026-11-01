/**
 * @file decomp_state.cpp
 * @brief Decomposition state and terminal-connectivity checks.
 */

#include "common/decomp_state.hpp"

#include <algorithm>
#include <numeric>
#include <set>
#include <sstream>

namespace teddy::graphrel {

decomp_state::decomp_state(const graph &net, const std::vector<std::size_t> &K)
    : remaining_edges(net.num_edges), uf_(net.num_vertices),
      current_K(K.begin(), K.end()) {
  std::iota(remaining_edges.begin(), remaining_edges.end(), std::size_t{0});
  std::sort(current_K.begin(), current_K.end());
}

decomp_state decomp_state::clone() const {
  decomp_state copy;
  copy.remaining_edges = remaining_edges;
  copy.uf_ = uf_;
  copy.current_K = current_K;
  copy.is_valid = is_valid;
  return copy;
}

bool decomp_state::is_success_terminal() const {
  if (current_K.size() < 2) {
    return true;
  }
  const std::size_t first_root = uf_.find_root(current_K.front());
  return std::all_of(current_K.begin(), current_K.end(), [&](std::size_t t) {
    return uf_.find_root(t) == first_root;
  });
}

bool decomp_state::is_failure_terminal(const graph &net,
                                       std::size_t ignored_edge_id) const {
  if (current_K.size() < 2 || is_success_terminal()) {
    return false;
  }
  // Merge along every remaining edge (except the ignored one) on a copy.
  union_find full = uf_;
  for (std::size_t edge_id : remaining_edges) {
    if (edge_id == ignored_edge_id) {
      continue;
    }
    const edge &e = net.edges[edge_id];
    full.unite(e.from, e.to);
  }
  const std::size_t first_root = full.find(current_K.front());
  return std::any_of(current_K.begin(), current_K.end(), [&](std::size_t t) {
    return full.find(t) != first_root;
  });
}

bool decomp_state::operator==(const decomp_state &other) const {
  return remaining_edges == other.remaining_edges && uf_ == other.uf_ &&
         current_K == other.current_K && is_valid == other.is_valid;
}

std::string decomp_state::to_string(const graph &net) const {
  std::ostringstream oss;
  oss << "decomp_state{\n  remaining_edges=[";
  for (std::size_t i = 0; i < remaining_edges.size(); ++i) {
    oss << (i > 0 ? "," : "") << remaining_edges[i];
  }
  oss << "]\n  union_find_components={";
  bool first = true;
  for (std::size_t v = 0; v < uf_.size(); ++v) {
    if (uf_.find_root(v) != v) {
      continue;
    }
    oss << (first ? "" : ",") << "v" << v << ":{";
    first = false;
    bool first_member = true;
    for (std::size_t u = 0; u < uf_.size(); ++u) {
      if (uf_.find_root(u) == v) {
        oss << (first_member ? "" : ",") << u;
        first_member = false;
      }
    }
    oss << "}";
  }
  oss << "}\n  current_K=[";
  for (std::size_t i = 0; i < current_K.size(); ++i) {
    oss << (i > 0 ? "," : "") << current_K[i];
  }
  oss << "]\n  boundary_vertex_roots={";
  std::set<std::size_t> boundary_vertices;
  for (std::size_t eid : remaining_edges) {
    if (eid < net.edges.size()) {
      boundary_vertices.insert(net.edges[eid].from);
      boundary_vertices.insert(net.edges[eid].to);
    }
  }
  first = true;
  for (std::size_t v : boundary_vertices) {
    oss << (first ? "" : ",") << "v" << v << "->" << uf_.find_root(v);
    first = false;
  }
  oss << "}\n  is_valid=" << (is_valid ? "true" : "false") << "\n}";
  return oss.str();
}

} // namespace teddy::graphrel
