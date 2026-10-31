/**
 * @file graph.cpp
 * @brief Graph structure implementation
 */

#include "teddy/graphrel/graph.hpp"
#include "common/validation.hpp"

#include <algorithm>
#include <stdexcept>

namespace teddy::graphrel {

std::vector<std::size_t> graph::get_neighbors(std::size_t v) const {
  if (v >= num_vertices) {
    throw std::out_of_range("Vertex index out of range");
  }

  std::vector<std::size_t> neighbors;
  neighbors.reserve(adj[v].size());

  for (std::size_t edge_id : adj[v]) {
    const edge &e = edges[edge_id];
    neighbors.push_back((e.from == v) ? e.to : e.from);
  }

  return neighbors;
}

std::vector<std::size_t> graph::get_incident_edges(std::size_t v) const {
  if (v >= num_vertices) {
    throw std::out_of_range("Vertex index out of range");
  }
  return adj[v];
}

std::size_t graph::add_edge(std::size_t from, std::size_t to, double prob) {
  if (from >= num_vertices || to >= num_vertices) {
    throw std::out_of_range("Vertex index out of range");
  }
  if (!(prob >= 0.0 && prob <= 1.0)) { // also rejects NaN
    throw std::invalid_argument("Edge probability must be in [0, 1]");
  }

  // Edge IDs are sequential and equal to the index in `edges`.
  std::size_t edge_id = edges.size();
  edges.emplace_back(edge_id, from, to, prob);

  adj[from].push_back(edge_id);
  if (from != to) { // a self-loop is listed once
    adj[to].push_back(edge_id);
  }

  return edge_id;
}

void graph::set_terminals(std::vector<std::size_t> k) {
  if (k.empty()) {
    throw std::invalid_argument("Terminal set must not be empty");
  }
  for (std::size_t v : k) {
    if (v >= num_vertices) {
      throw std::out_of_range("Terminal vertex index out of range");
    }
  }
  std::sort(k.begin(), k.end());
  k.erase(std::unique(k.begin(), k.end()), k.end());
  terminals = std::move(k);
}

bool graph::validate() const {
  return detail::find_graph_problem(*this) == nullptr;
}

} // namespace teddy::graphrel
