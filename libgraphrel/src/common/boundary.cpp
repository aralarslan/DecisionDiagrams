/**
 * @file boundary.cpp
 * @brief boundary_computer: shared boundary partition computation
 */

#include "common/boundary.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace teddy::graphrel {

namespace {
constexpr std::size_t not_in_ordering = std::numeric_limits<std::size_t>::max();
}

boundary_computer::boundary_computer(const graph& g,
                                     const std::vector<variable>& ordering)
    : net_(g)
    , edge_level_(g.num_edges, not_in_ordering)
    , vertex_first_level_(g.num_vertices, not_in_ordering)
{
    for (std::size_t level = 0; level < ordering.size(); ++level) {
        const variable& var = ordering[level];
        if (var.type == variable_type::edge) {
            edge_level_[var.id] = level;
            const edge& e = g.edges[var.id];
            vertex_first_level_[e.from] = std::min(vertex_first_level_[e.from], level);
            vertex_first_level_[e.to]   = std::min(vertex_first_level_[e.to], level);
        } else {
            vertex_first_level_[var.id] = std::min(vertex_first_level_[var.id], level);
        }
    }

    // Static front size per level: v is on the front at level l when
    // first_level(v) < l <= last_edge_level(v). Sweep a difference array.
    const std::size_t levels = ordering.size() + 1;
    std::vector<std::ptrdiff_t> delta(levels + 1, 0);
    for (std::size_t v = 0; v < g.num_vertices; ++v) {
        std::size_t last = not_in_ordering;
        for (std::size_t eid : g.adj[v]) {
            const std::size_t el = edge_level_[eid];
            if (el != not_in_ordering)
                last = (last == not_in_ordering) ? el : std::max(last, el);
        }
        const std::size_t first = vertex_first_level_[v];
        if (last == not_in_ordering || first == not_in_ordering || first >= last)
            continue;
        delta[first + 1] += 1;
        delta[last + 1] -= 1;
    }
    std::ptrdiff_t running = 0;
    for (std::size_t l = 0; l < levels; ++l) {
        running += delta[l];
        max_static_boundary_ = std::max(max_static_boundary_,
                                        static_cast<std::size_t>(running));
    }
}

boundary_partition boundary_computer::compute(const decomp_state& sg,
                                              std::size_t level,
                                              const bell_cache& cache) const {
    // Thread-local scratch: marks are cleared sparsely after each call.
    static thread_local std::vector<char> marked;
    static thread_local std::vector<std::size_t> boundary;
    if (marked.size() < net_.num_vertices)
        marked.resize(net_.num_vertices, 0);
    boundary.clear();

    for (std::size_t edge_id : sg.remaining_edges) {
        const std::size_t el = edge_level_[edge_id];
        if (el == not_in_ordering || el < level)
            continue; // processed edge (or not part of the ordering)
        const edge& e = net_.edges[edge_id];
        for (std::size_t v : {e.from, e.to}) {
            if (!marked[v] && vertex_first_level_[v] < level) {
                marked[v] = 1;
                boundary.push_back(v);
            }
        }
    }
    for (std::size_t v : boundary)
        marked[v] = 0;

    return canonicalize_partition(boundary, sg.uf_, sg.current_K, cache);
}

} // namespace teddy::graphrel
