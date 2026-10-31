/**
 * @file variable_ordering.cpp
 * @brief Variable ordering generation for network decomposition
 *
 * Implements variable ordering strategies for the layer-by-layer algorithm.
 * The ordering determines the sequence in which components (vertices and edges)
 * are processed during Shannon decomposition.
 *
 * Key Principle: Vertex Priority
 * Vertices are processed before their incident edges. This minimizes the
 * boundary set size F_max by ensuring edges are processed only after both
 * endpoints are known, reducing the number of vertices that are incident to
 * both processed and unprocessed edges.
 *
 * Strategies:
 * - BFS (Breadth-First Search): Default, minimizes F_max for most networks
 * - DFS (Depth-First Search): Alternative strategy for comparison
 *
 * @see Wu & Sun (2024) for vertex priority principle and ordering heuristics
 */

#include "teddy/graphrel/variable_ordering.hpp"
#include "common/validation.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <set>
#include <vector>

namespace teddy::graphrel {

std::vector<variable> generate_variable_ordering_dfs(const graph &g) {
  /**
   * @brief Generate variable ordering using Depth-First Search (DFS)
   *
   * Creates a variable ordering using DFS traversal with vertex priority.
   * This is an alternative to BFS that may produce different orderings for
   * testing ordering independence.
   *
   * Algorithm:
   * 1. Start DFS from a terminal vertex
   * 2. For each vertex visited:
   *    a. Add vertex to ordering (vertex priority)
   *    b. Add all edges with both endpoints processed
   *    c. Recursively visit unvisited neighbors
   * 3. Handle disconnected components
   *
   * @param g Graph structure
   * @return Variable ordering (vertices before their incident edges)
   */
  std::vector<variable> ordering;
  ordering.reserve(g.num_vertices + g.num_edges);

  std::set<std::size_t> processed_vertices; // Vertices added to ordering
  std::set<std::size_t> visited_edges;

  // Helper function: Add edges that have both endpoints processed
  // This implements the vertex priority principle: edges are only added
  // after both endpoints have been processed
  auto add_ready_edges = [&]() {
    for (std::size_t edge_id = 0; edge_id < g.num_edges; ++edge_id) {
      if (visited_edges.find(edge_id) == visited_edges.end()) {
        const edge &e = g.edges[edge_id];
        // Only add edge if both endpoints are processed
        if (processed_vertices.find(e.from) != processed_vertices.end() &&
            processed_vertices.find(e.to) != processed_vertices.end()) {
          ordering.emplace_back(variable_type::edge, edge_id);
          visited_edges.insert(edge_id);
        }
      }
    }
  };

  std::function<void(std::size_t)> dfs = [&](std::size_t v) {
    // Add the vertex to ordering (vertex-first heuristic)
    ordering.emplace_back(variable_type::vertex, v);
    processed_vertices.insert(v); // Mark as processed

    // Add edges that now have both endpoints processed (vertex priority
    // principle)
    add_ready_edges();

    // Recursively visit unvisited neighbors (DFS)
    for (std::size_t edge_id : g.adj[v]) {
      const edge &e = g.edges[edge_id];
      std::size_t neighbor = (e.from == v) ? e.to : e.from;
      if (processed_vertices.find(neighbor) == processed_vertices.end()) {
        dfs(neighbor);
      }
    }
  };

  // Start DFS from a terminal vertex
  if (!g.terminals.empty()) {
    std::size_t seed = g.terminals.front();
    dfs(seed);
  } else if (g.num_vertices > 0) {
    dfs(0);
  }

  // Add any disconnected components
  for (std::size_t i = 0; i < g.num_vertices; ++i) {
    if (processed_vertices.find(i) == processed_vertices.end()) {
      dfs(i);
    }
  }

  // Add any remaining edges (shouldn't happen if vertex priority is followed
  // correctly)
  add_ready_edges();

  return ordering;
}

std::vector<variable> generate_variable_ordering(const graph &g) {
  /**
   * @brief Generate variable ordering using Breadth-First Search (BFS)
   *
   * Creates a variable ordering using BFS traversal with vertex priority.
   * This is the default ordering strategy recommended by Wu & Sun (2024).
   *
   * Vertex Priority Principle:
   * - Process vertices before their incident edges
   * - Add an edge to ordering only when BOTH endpoints have been processed
   * - This minimizes boundary set size F_max by reducing vertices that are
   *   incident to both processed and unprocessed edges
   *
   * Algorithm:
   * 1. Start BFS from a single terminal vertex (seed)
   * 2. For each vertex in BFS queue:
   *    a. Add vertex to ordering
   *    b. Queue unvisited neighbors
   *    c. Add all edges (network-wide) with both endpoints processed
   * 3. Handle disconnected components
   *
   * IMPORTANT: Start from ONE terminal, not all terminals. Starting from
   * all terminals simultaneously can cause incorrect ordering where
   * intermediate vertices are processed after edges that depend on them.
   *
   * @param g Graph structure
   * @return Variable ordering following vertex priority principle
   */
  detail::require_valid(g, "generate_variable_ordering");
  std::vector<variable> ordering;
  ordering.reserve(g.num_vertices + g.num_edges);

  std::set<std::size_t> processed_vertices; // Vertices added to ordering
  std::set<std::size_t>
      queued_vertices; // Vertices queued (to prevent duplicates)
  std::set<std::size_t> visited_edges;
  std::queue<std::size_t> q;

  // Start BFS from a single seed vertex (one terminal if available)
  // CRITICAL: Use only ONE terminal as seed, not all terminals.
  // Adding ALL terminals at once causes incorrect ordering where intermediate
  // vertices are processed after edges that depend on them, violating vertex
  // priority and increasing F_max.
  if (!g.terminals.empty()) {
    std::size_t seed = g.terminals.front(); // Pick first terminal
    q.push(seed);
    queued_vertices.insert(seed);
  } else if (g.num_vertices > 0) {
    q.push(0);
    queued_vertices.insert(0);
  }

  while (!q.empty()) {
    std::size_t v = q.front();
    q.pop();

    // Step 1: Add vertex to ordering (vertex priority - vertices come first)
    ordering.emplace_back(variable_type::vertex, v);
    processed_vertices.insert(v); // Mark as processed

    // Step 2: Queue unvisited neighbors for BFS traversal
    // Mark neighbors as queued to prevent duplicate enqueueing
    for (std::size_t edge_id : g.adj[v]) {
      const edge &e = g.edges[edge_id];
      std::size_t neighbor = (e.from == v) ? e.to : e.from;
      if (queued_vertices.find(neighbor) == queued_vertices.end()) {
        q.push(neighbor);
        queued_vertices.insert(neighbor); // Mark as queued
      }
    }

    // Step 3: Add edges with both endpoints processed (vertex priority
    // principle) IMPORTANT: Check ALL edges in the network, not just incident
    // edges. This ensures edges are added as soon as both endpoints are
    // processed, regardless of which vertex triggered the check. Example: v1 ->
    // v2 -> e1 (e1 added after both v1 and v2 are processed)
    for (std::size_t edge_id = 0; edge_id < g.num_edges; ++edge_id) {
      if (visited_edges.find(edge_id) == visited_edges.end()) {
        const edge &e = g.edges[edge_id];
        // Vertex Priority: Only add edge if BOTH endpoints are processed
        // This minimizes boundary set size by ensuring edges are processed
        // immediately after their endpoints, not later
        if (processed_vertices.find(e.from) != processed_vertices.end() &&
            processed_vertices.find(e.to) != processed_vertices.end()) {
          ordering.emplace_back(variable_type::edge, edge_id);
          visited_edges.insert(edge_id);
        }
      }
    }
  }

  // Add any disconnected components
  for (std::size_t i = 0; i < g.num_vertices; ++i) {
    if (queued_vertices.find(i) == queued_vertices.end()) {
      q.push(i);
      queued_vertices.insert(i);
      while (!q.empty()) {
        std::size_t v = q.front();
        q.pop();
        ordering.emplace_back(variable_type::vertex, v);
        processed_vertices.insert(v);
        for (std::size_t edge_id : g.adj[v]) {
          const edge &e = g.edges[edge_id];
          std::size_t neighbor = (e.from == v) ? e.to : e.from;
          if (queued_vertices.find(neighbor) == queued_vertices.end()) {
            q.push(neighbor);
            queued_vertices.insert(neighbor);
          }
          if (visited_edges.find(edge_id) == visited_edges.end() &&
              processed_vertices.find(neighbor) != processed_vertices.end()) {
            ordering.emplace_back(variable_type::edge, edge_id);
            visited_edges.insert(edge_id);
          }
        }
      }
    }
  }

  return ordering;
}

std::vector<variable> generate_variable_ordering(const graph &g,
                                                 ordering_strategy strategy) {
  detail::require_valid(g, "generate_variable_ordering");
  switch (strategy) {
  case ordering_strategy::bfs:
    return generate_variable_ordering(g);
  case ordering_strategy::dfs:
    return generate_variable_ordering_dfs(g);
  default:
    return generate_variable_ordering(g);
  }
}

std::size_t estimate_max_boundary_size(const graph &g,
                                       const std::vector<variable> &ordering) {
  /**
   * @brief Estimate maximum boundary set size F_max for given ordering
   *
   * Simulates the decomposition process and tracks the boundary set size
   * at each level. The boundary set F at level l contains vertices incident
   * to BOTH processed and unprocessed edges.
   *
   * This function is useful for:
   * - Comparing different ordering strategies (BFS vs DFS)
   * - Validating that vertex priority minimizes F_max
   * - Performance estimation (memory usage scales with F_max)
   *
   * @param g Graph structure
   * @param ordering Variable ordering to analyze
   * @return Maximum boundary set size F_max across all levels
   */
  std::size_t max_boundary = 0;
  std::set<std::size_t> processed_vars;

  // Simulate decomposition level by level
  for (std::size_t level = 0; level < ordering.size(); ++level) {
    // Variable at this level (not used in boundary estimation)
    // const variable& var = ordering[level];
    processed_vars.insert(level);

    // Compute boundary set at this level
    // Boundary = {v | v incident to processed AND unprocessed edges}
    std::set<std::size_t> boundary;

    for (std::size_t v = 0; v < g.num_vertices; ++v) {
      bool has_processed = false;
      bool has_unprocessed = false;

      // Check if vertex v is incident to processed and unprocessed edges
      for (std::size_t edge_id : g.adj[v]) {
        // Find this edge's position in the ordering
        auto it = std::find_if(
            ordering.begin(), ordering.end(), [edge_id](const variable &var) {
              return var.type == variable_type::edge && var.id == edge_id;
            });

        if (it != ordering.end()) {
          std::size_t edge_level = std::distance(ordering.begin(), it);
          if (edge_level <= level) {
            has_processed = true; // Edge processed at or before current level
          } else {
            has_unprocessed = true; // Edge not yet processed
          }
        }
      }

      // Vertex is in boundary if it has both processed and unprocessed edges
      if (has_processed && has_unprocessed) {
        boundary.insert(v);
      }
    }

    // Track maximum boundary size across all levels
    max_boundary = std::max(max_boundary, boundary.size());
  }

  return max_boundary;
}

} // namespace teddy::graphrel
