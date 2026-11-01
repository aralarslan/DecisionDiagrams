/**
 * @file pruning.cpp
 * @brief Dead component pruning (common free function)
 *
 * Shared by the LBL calculator and the BDD builder.
 */

#include "common/pruning.hpp"
#include <algorithm>
#include <vector>

namespace teddy::graphrel {

void prune_unreachable_edges(decomp_state &state, const graph &net) {
  if (state.current_K.empty() || state.remaining_edges.empty()) {
    return;
  }

  const auto &terminals = state.current_K;

  std::vector<bool> comp_has_terminal(net.num_vertices);
  std::vector<int> comp_degree(net.num_vertices);

  bool changed = true;
  while (changed) {
    changed = false;

    std::fill(comp_has_terminal.begin(), comp_has_terminal.end(), false);
    std::fill(comp_degree.begin(), comp_degree.end(), 0);

    for (std::size_t t : terminals) {
      comp_has_terminal[state.uf_.find_root(t)] = true;
    }

    for (std::size_t edge_id : state.remaining_edges) {
      const auto &e = net.edges[edge_id];
      std::size_t r1 = state.uf_.find_root(e.from);
      std::size_t r2 = state.uf_.find_root(e.to);
      comp_degree[r1]++;
      if (r1 != r2) {
        comp_degree[r2]++;
      }
    }

    auto is_dead_component = [&](std::size_t root) {
      return !comp_has_terminal[root] && comp_degree[root] <= 1;
    };

    std::vector<std::size_t> edges_to_prune;
    for (std::size_t edge_id : state.remaining_edges) {
      const auto &e = net.edges[edge_id];
      std::size_t r1 = state.uf_.find_root(e.from);
      std::size_t r2 = state.uf_.find_root(e.to);

      if (is_dead_component(r1) || is_dead_component(r2)) {
        edges_to_prune.push_back(edge_id);
        comp_degree[r1]--;
        comp_degree[r2]--;
        changed = true;
      }
    }

    if (!edges_to_prune.empty()) {
      auto new_end = std::remove_if(state.remaining_edges.begin(),
                                    state.remaining_edges.end(),
                                    [&edges_to_prune](std::size_t edge_id) {
                                      return std::binary_search(
                                          edges_to_prune.begin(),
                                          edges_to_prune.end(), edge_id);
                                    });
      state.remaining_edges.erase(new_end, state.remaining_edges.end());
    }
  }
}

} // namespace teddy::graphrel
