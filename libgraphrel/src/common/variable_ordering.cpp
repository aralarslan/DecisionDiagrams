/**
 * @file variable_ordering.cpp
 * @brief Variable ordering generation for network decomposition
 *
 * Orderings follow the vertex priority principle (Wu & Sun 2024, §2.3;
 * Kawahara et al. 2019, §3.1): every vertex appears before its incident
 * edges, and an edge is emitted as soon as both endpoints have been emitted.
 *
 * - BFS (default): breadth-first from the first terminal.
 * - DFS: depth-first from the first terminal. Equally correct, but it can
 *   produce far larger boundary sets (and layers) than BFS.
 *
 * Both run in O(V + E). When a vertex is emitted, the edges that become ready
 * are exactly its incident edges whose other endpoint was already emitted,
 * taken in adjacency (= edge id) order.
 */

#include "teddy/graphrel/variable_ordering.hpp"
#include "common/validation.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace teddy::graphrel {

namespace {

/// Emits vertices and their newly ready edges, tracking what was emitted.
class ordering_builder {
public:
    explicit ordering_builder(const graph& g)
        : g_(g), emitted_vertex_(g.num_vertices, 0), emitted_edge_(g.num_edges, 0) {
        ordering_.reserve(g.num_vertices + g.num_edges);
    }

    bool emitted(std::size_t v) const { return emitted_vertex_[v] != 0; }

    void emit_vertex(std::size_t v) {
        ordering_.emplace_back(variable_type::vertex, v);
        emitted_vertex_[v] = 1;
        for (std::size_t eid : g_.adj[v]) {
            if (emitted_edge_[eid])
                continue;
            const edge& e = g_.edges[eid];
            const std::size_t other = (e.from == v) ? e.to : e.from;
            if (emitted_vertex_[other]) { // includes self-loops
                ordering_.emplace_back(variable_type::edge, eid);
                emitted_edge_[eid] = 1;
            }
        }
    }

    std::vector<variable> take() { return std::move(ordering_); }

private:
    const graph& g_;
    std::vector<char> emitted_vertex_;
    std::vector<char> emitted_edge_;
    std::vector<variable> ordering_;
};

std::size_t neighbor(const graph& g, std::size_t eid, std::size_t v) {
    const edge& e = g.edges[eid];
    return (e.from == v) ? e.to : e.from;
}

std::vector<variable> bfs_ordering(const graph& g) {
    ordering_builder out(g);
    std::vector<char> queued(g.num_vertices, 0);
    std::queue<std::size_t> q;

    auto run_from = [&](std::size_t seed) {
        q.push(seed);
        queued[seed] = 1;
        while (!q.empty()) {
            const std::size_t v = q.front();
            q.pop();
            for (std::size_t eid : g.adj[v]) {
                const std::size_t nb = neighbor(g, eid, v);
                if (!queued[nb]) {
                    queued[nb] = 1;
                    q.push(nb);
                }
            }
            out.emit_vertex(v);
        }
    };

    // Seed with ONE terminal (Wu & Sun 2024, Fig. 4 step 1). Seeding with all
    // terminals at once would interleave fronts and enlarge the boundary.
    run_from(g.terminals.empty() ? 0 : g.terminals.front());
    for (std::size_t v = 0; v < g.num_vertices; ++v) // other components
        if (!queued[v])
            run_from(v);
    return out.take();
}

std::vector<variable> dfs_ordering(const graph& g) {
    ordering_builder out(g);
    // Explicit stack of (vertex, next adjacency index): same visiting order as
    // the recursive formulation, without risking stack overflow on long paths.
    std::vector<std::pair<std::size_t, std::size_t>> stack;

    auto run_from = [&](std::size_t seed) {
        out.emit_vertex(seed);
        stack.emplace_back(seed, 0);
        while (!stack.empty()) {
            auto& [v, next] = stack.back();
            if (next == g.adj[v].size()) {
                stack.pop_back();
                continue;
            }
            const std::size_t nb = neighbor(g, g.adj[v][next++], v);
            if (!out.emitted(nb)) {
                out.emit_vertex(nb);
                stack.emplace_back(nb, 0);
            }
        }
    };

    run_from(g.terminals.empty() ? 0 : g.terminals.front());
    for (std::size_t v = 0; v < g.num_vertices; ++v) // other components
        if (!out.emitted(v))
            run_from(v);
    return out.take();
}

} // namespace

std::vector<variable> generate_variable_ordering(const graph& g) {
    detail::require_valid(g, "generate_variable_ordering");
    return bfs_ordering(g);
}

std::vector<variable> generate_variable_ordering(const graph& g,
                                                 ordering_strategy strategy) {
    detail::require_valid(g, "generate_variable_ordering");
    switch (strategy) {
    case ordering_strategy::bfs:
        return bfs_ordering(g);
    case ordering_strategy::dfs:
        return dfs_ordering(g);
    }
    throw std::invalid_argument("generate_variable_ordering: unknown strategy");
}

std::size_t estimate_max_boundary_size(const graph& g,
                                       const std::vector<variable>& ordering) {
    // Boundary at level l: vertices with at least one processed and at least
    // one unprocessed incident edge, where an edge is processed once its
    // position in the ordering is <= l. Edges absent from the ordering are
    // ignored. Maintained incrementally: O(V + E).
    detail::require_valid(g, "estimate_max_boundary_size");
    constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();

    std::vector<std::size_t> edge_pos(g.num_edges, absent);
    for (std::size_t pos = 0; pos < ordering.size(); ++pos) {
        const variable& var = ordering[pos];
        const std::size_t limit =
            var.type == variable_type::edge ? g.num_edges : g.num_vertices;
        if (var.id >= limit)
            throw std::invalid_argument(
                "estimate_max_boundary_size: variable id " +
                std::to_string(var.id) + " out of range");
        if (var.type == variable_type::edge && edge_pos[var.id] == absent)
            edge_pos[var.id] = pos;
    }

    std::vector<std::size_t> total(g.num_vertices, 0), done(g.num_vertices, 0);
    for (std::size_t v = 0; v < g.num_vertices; ++v)
        for (std::size_t eid : g.adj[v])
            total[v] += (edge_pos[eid] != absent);

    auto on_boundary = [&](std::size_t v) { return done[v] > 0 && done[v] < total[v]; };

    std::size_t current = 0, max_boundary = 0;
    for (std::size_t level = 0; level < ordering.size(); ++level) {
        const variable& var = ordering[level];
        if (var.type == variable_type::edge && edge_pos[var.id] == level) {
            const edge& e = g.edges[var.id];
            auto process_endpoint = [&](std::size_t v) {
                const bool before = on_boundary(v);
                ++done[v];
                const bool after = on_boundary(v);
                current = current + (after ? 1 : 0) - (before ? 1 : 0);
            };
            process_endpoint(e.from);
            if (e.to != e.from) // a self-loop has one adjacency entry
                process_endpoint(e.to);
        }
        max_boundary = std::max(max_boundary, current);
    }
    return max_boundary;
}

} // namespace teddy::graphrel
