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
 * - cutwidth: greedy edge-cut minimisation with candidate selection; the
 *   default for calculate_reliability() and build_diagram().
 *
 * When a vertex is emitted, the edges that become ready are exactly its
 * incident edges whose other endpoint was already emitted. BFS and DFS take
 * them in adjacency (= edge id) order, as in Wu & Sun's experiments; both run
 * in O(V + E). The cut-width ordering sorts them (edge_order::departing_first,
 * see emit_vertex), which changes the layer sizes but never the result.
 */

#include "teddy/graphrel/variable_ordering.hpp"
#include "common/validation.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace teddy::graphrel {

namespace {

/// Order of the edges that become ready when a vertex is emitted.
enum class edge_order {
    by_id,          ///< adjacency (= edge id) order
    departing_first ///< see ordering_builder::emit_vertex
};

/// Emits vertices and their newly ready edges, tracking what was emitted.
class ordering_builder {
public:
    ordering_builder(const graph& g, edge_order order)
        : g_(g)
        , order_(order)
        , emitted_vertex_(g.num_vertices(), 0)
        , emitted_edge_(g.num_edges(), 0)
        , remaining_(g.num_vertices(), 0)
        , emitted_at_(g.num_vertices(), 0) {
        ordering_.reserve(g.num_vertices() + g.num_edges());
        for (const edge& e : g.edges()) {
            if (e.from != e.to) {
                ++remaining_[e.from];
                ++remaining_[e.to];
            }
        }
    }

    bool emitted(std::size_t v) const { return emitted_vertex_[v] != 0; }

    /// Emit @p v and then its ready edges. With edge_order::departing_first
    /// the edges are sorted by the other endpoint: fewest unemitted edges
    /// first (an endpoint whose last edge this is leaves the front at once,
    /// closing or pruning its block before the next branching), then the
    /// most recently emitted endpoint first. Measured over meshes, complete
    /// and random geometric graphs, this never enlarged the widest layer and
    /// shrank it up to 2.2x (libgraphrel/audit/HANDOFF.md, §5.6).
    void emit_vertex(std::size_t v) {
        ordering_.emplace_back(variable_type::vertex, v);
        emitted_vertex_[v] = 1;
        emitted_at_[v] = ++clock_;
        ready_.clear();
        for (std::size_t eid : g_.get_incident_edges(v)) {
            if (emitted_edge_[eid])
                continue;
            if (emitted_vertex_[neighbor_of(eid, v)]) { // includes self-loops
                ready_.push_back(eid);
                emitted_edge_[eid] = 1;
            }
        }
        if (order_ == edge_order::departing_first) {
            std::stable_sort(ready_.begin(), ready_.end(), [&](std::size_t a, std::size_t b) {
                const std::size_t ua = neighbor_of(a, v), ub = neighbor_of(b, v);
                if (remaining_[ua] != remaining_[ub])
                    return remaining_[ua] < remaining_[ub];
                return emitted_at_[ua] > emitted_at_[ub];
            });
        }
        for (std::size_t eid : ready_) {
            ordering_.emplace_back(variable_type::edge, eid);
            const edge& e = g_.edges()[eid];
            if (e.from != e.to) {
                --remaining_[e.from];
                --remaining_[e.to];
            }
        }
    }

    std::vector<variable> take() { return std::move(ordering_); }

private:
    std::size_t neighbor_of(std::size_t eid, std::size_t v) const {
        const edge& e = g_.edges()[eid];
        return (e.from == v) ? e.to : e.from;
    }

    const graph& g_;
    edge_order order_;
    std::vector<char> emitted_vertex_;
    std::vector<char> emitted_edge_;
    std::vector<std::size_t> remaining_;  ///< unemitted non-loop edges per vertex
    std::vector<std::size_t> emitted_at_; ///< emission time per vertex (0 = not yet)
    std::size_t clock_ = 0;
    std::vector<std::size_t> ready_;
    std::vector<variable> ordering_;
};

std::size_t neighbor(const graph& g, std::size_t eid, std::size_t v) {
    const edge& e = g.edges()[eid];
    return (e.from == v) ? e.to : e.from;
}

std::vector<variable> bfs_ordering(const graph& g) {
    ordering_builder out(g, edge_order::by_id);
    std::vector<char> queued(g.num_vertices(), 0);
    std::queue<std::size_t> q;

    auto run_from = [&](std::size_t seed) {
        q.push(seed);
        queued[seed] = 1;
        while (!q.empty()) {
            const std::size_t v = q.front();
            q.pop();
            for (std::size_t eid : g.get_incident_edges(v)) {
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
    run_from(g.terminals().empty() ? 0 : g.terminals().front());
    for (std::size_t v = 0; v < g.num_vertices(); ++v) // other components
        if (!queued[v])
            run_from(v);
    return out.take();
}

std::vector<variable> dfs_ordering(const graph& g) {
    ordering_builder out(g, edge_order::by_id);
    // Explicit stack of (vertex, next adjacency index): same visiting order as
    // the recursive formulation, without risking stack overflow on long paths.
    std::vector<std::pair<std::size_t, std::size_t>> stack;

    auto run_from = [&](std::size_t seed) {
        out.emit_vertex(seed);
        stack.emplace_back(seed, 0);
        while (!stack.empty()) {
            auto& [v, next] = stack.back();
            if (next == g.get_incident_edges(v).size()) {
                stack.pop_back();
                continue;
            }
            const std::size_t nb = neighbor(g, g.get_incident_edges(v)[next++], v);
            if (!out.emitted(nb)) {
                out.emit_vertex(nb);
                stack.emplace_back(nb, 0);
            }
        }
    };

    run_from(g.terminals().empty() ? 0 : g.terminals().front());
    for (std::size_t v = 0; v < g.num_vertices(); ++v) // other components
        if (!out.emitted(v))
            run_from(v);
    return out.take();
}

// ── Cut-width ordering ───────────────────────────────────────────────────────
//
// The number of LBL states per layer is driven less by the boundary size F
// than by the number of unprocessed edges leaving the processed part (the
// edge cut). With a small cut, most boundary vertices have a single remaining
// edge, so non-terminal components touching the boundary once are pruned as
// dead ends and more states merge. Example (10x10 mesh, p = 0.9, exact):
// BFS sweeps diagonally (cut 18, 387,481 states); a cut-width order sweeps
// row by row (cut 11, F = 11 for both). See libgraphrel/audit/HANDOFF.md.

std::size_t non_loop_neighbor(const graph& g, std::size_t eid, std::size_t v) {
    return neighbor(g, eid, v);
}

/// Greedy vertex sequence: repeatedly take the reached vertex whose addition
/// grows the edge cut least (deg(v) - 2 * processed_neighbours(v)), preferring
/// the most recently touched vertex (continue the current line), then the
/// lowest id. @p first, if given, is forced as the second vertex.
std::vector<std::size_t> greedy_cut_sequence(const graph& g, std::size_t seed,
                                             std::size_t first) {
    constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    const std::size_t n = g.num_vertices();
    std::vector<long long> deg(n, 0), processed_nb(n, 0), touched(n, -1);
    for (std::size_t v = 0; v < n; ++v)
        for (std::size_t eid : g.get_incident_edges(v))
            if (g.edges()[eid].from != g.edges()[eid].to)
                ++deg[v];

    // Min-heap on (cut growth, -touch time, id) with lazy invalidation.
    struct entry {
        long long growth;
        long long neg_touch;
        std::size_t id;
        bool operator>(const entry& o) const {
            return std::tie(growth, neg_touch, id) > std::tie(o.growth, o.neg_touch, o.id);
        }
    };
    std::priority_queue<entry, std::vector<entry>, std::greater<entry>> heap;
    std::vector<char> done(n, 0);
    std::vector<std::size_t> sequence;
    sequence.reserve(n);
    long long clock = 0;

    auto take = [&](std::size_t v) {
        done[v] = 1;
        sequence.push_back(v);
        ++clock;
        for (std::size_t eid : g.get_incident_edges(v)) {
            const std::size_t o = non_loop_neighbor(g, eid, v);
            if (o == v || done[o])
                continue;
            ++processed_nb[o];
            touched[o] = clock;
            heap.push({deg[o] - 2 * processed_nb[o], -touched[o], o});
        }
    };
    auto pop_valid = [&]() -> std::size_t {
        while (!heap.empty()) {
            const entry top = heap.top();
            heap.pop();
            const std::size_t v = top.id;
            if (!done[v] && top.growth == deg[v] - 2 * processed_nb[v] &&
                top.neg_touch == -touched[v])
                return v;
        }
        return none;
    };

    take(seed);
    if (first != none && !done[first])
        take(first);
    std::size_t next_unvisited = 0;
    while (sequence.size() < n) {
        std::size_t v = pop_valid();
        if (v == none) { // new component: lowest unvisited id
            while (done[next_unvisited])
                ++next_unvisited;
            v = next_unvisited;
        }
        take(v);
    }
    return sequence;
}

struct cut_profile {
    std::size_t max_cut = 0;     ///< largest edge cut over the sequence
    std::size_t max_front = 0;   ///< largest vertex boundary
    std::size_t total_cut = 0;   ///< sum of cuts (tie-breaker)
    bool operator<(const cut_profile& o) const {
        return std::tie(max_cut, max_front, total_cut) <
               std::tie(o.max_cut, o.max_front, o.total_cut);
    }
};

cut_profile profile_of(const graph& g, const std::vector<std::size_t>& sequence) {
    std::vector<char> done(g.num_vertices(), 0);
    std::vector<std::size_t> open_edges(g.num_vertices(), 0);
    for (std::size_t v = 0; v < g.num_vertices(); ++v)
        for (std::size_t eid : g.get_incident_edges(v))
            if (g.edges()[eid].from != g.edges()[eid].to)
                ++open_edges[v];
    cut_profile p;
    std::size_t cut = 0, front = 0;
    for (std::size_t v : sequence) {
        done[v] = 1;
        for (std::size_t eid : g.get_incident_edges(v)) {
            const std::size_t o = non_loop_neighbor(g, eid, v);
            if (o == v)
                continue;
            if (done[o]) {
                --cut;
                --open_edges[v];
                if (--open_edges[o] == 0)
                    --front;
            } else {
                ++cut;
            }
        }
        if (open_edges[v] > 0)
            ++front;
        p.max_cut = std::max(p.max_cut, cut);
        p.max_front = std::max(p.max_front, front);
        p.total_cut += cut;
    }
    return p;
}

std::vector<std::size_t> vertices_of(const std::vector<variable>& ordering) {
    std::vector<std::size_t> sequence;
    for (const variable& var : ordering)
        if (var.type == variable_type::vertex)
            sequence.push_back(var.id);
    return sequence;
}

/// Estimated LBL work of an ordering when every vertex is perfect: the sum
/// over levels of 2^(a * F), F = front size before the level. With perfect
/// vertices a front vertex is absent only once pruned, so the layer size
/// follows the number of block partitions of the front, and the edge cut adds
/// little: fitted within graphs over 1,900 levels (meshes, random geometric
/// graphs, BFS / DFS / cut-width), log2(states) = 1.61 F - 0.015 C + c(graph)
/// with perfect vertices, against 1.51 F + 0.10 C + c(graph) with imperfect
/// ones. The choice between candidates is insensitive to a in [1, 2].
double perfect_vertex_work(const graph& g, const std::vector<variable>& ordering) {
    constexpr double a = 1.6;
    std::vector<std::size_t> remaining(g.num_vertices(), 0);
    for (const edge& e : g.edges()) {
        if (e.from != e.to) {
            ++remaining[e.from];
            ++remaining[e.to];
        }
    }
    std::size_t front = 0;
    double work = 0.0;
    for (const variable& var : ordering) {
        work += std::exp2(a * static_cast<double>(front));
        if (var.type == variable_type::vertex) {
            front += remaining[var.id] > 0;
        } else {
            const edge& e = g.edges()[var.id];
            if (e.from != e.to) {
                front -= --remaining[e.from] == 0;
                front -= --remaining[e.to] == 0;
            }
        }
    }
    return work;
}

std::vector<variable> emit_departing_first(const graph& g, const std::vector<std::size_t>& sequence) {
    ordering_builder out(g, edge_order::departing_first);
    for (std::size_t v : sequence)
        out.emit_vertex(v);
    return out.take();
}

std::vector<variable> cutwidth_ordering(const graph& g) {
    constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    constexpr std::size_t max_forced_starts = 16;
    const std::size_t seed = g.terminals().empty() ? 0 : g.terminals().front();

    // Candidates: BFS (so the result is never worse than BFS on this metric),
    // the free greedy, and the greedy forced through each neighbour of the
    // seed (this picks the sweep direction, e.g. the short side of a mesh).
    std::vector<std::vector<std::size_t>> candidates;
    candidates.push_back(vertices_of(bfs_ordering(g)));
    candidates.push_back(greedy_cut_sequence(g, seed, none));
    std::vector<char> tried(g.num_vertices(), 0);
    std::size_t forced = 0;
    for (std::size_t eid : g.get_incident_edges(seed)) {
        const std::size_t nb = neighbor(g, eid, seed);
        if (nb == seed || tried[nb] || forced == max_forced_starts)
            continue;
        tried[nb] = 1;
        ++forced;
        candidates.push_back(greedy_cut_sequence(g, seed, nb));
    }

    // Selection. With imperfect vertices, the edge cut governs the layer
    // size (a front vertex with two or more unprocessed edges cannot be
    // pruned when absent or isolated): smallest (largest cut, largest front,
    // total cut). With perfect vertices only the front size matters, and
    // ties on the peak are common (a square mesh swept by rows or by
    // diagonals has the same widest layer, but the diagonal sweep is only
    // wide in the middle: 10x10, 2.9 M vs 8.4 M states in total), so the
    // estimated total work decides. The first candidate wins ties.
    const bool perfect_vertices =
        std::all_of(g.vertex_probs().begin(), g.vertex_probs().end(), [](double p) { return p >= 1.0; });
    std::size_t best = 0;
    if (perfect_vertices) {
        double best_work = 0.0;
        for (std::size_t c = 0; c < candidates.size(); ++c) {
            const double work = perfect_vertex_work(g, emit_departing_first(g, candidates[c]));
            if (c == 0 || work < best_work) {
                best_work = work;
                best = c;
            }
        }
    } else {
        cut_profile best_profile = profile_of(g, candidates[0]);
        for (std::size_t c = 1; c < candidates.size(); ++c) {
            const cut_profile p = profile_of(g, candidates[c]);
            if (p < best_profile) {
                best_profile = p;
                best = c;
            }
        }
    }
    return emit_departing_first(g, candidates[best]);
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
    case ordering_strategy::cutwidth:
        return cutwidth_ordering(g);
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

    std::vector<std::size_t> edge_pos(g.num_edges(), absent);
    for (std::size_t pos = 0; pos < ordering.size(); ++pos) {
        const variable& var = ordering[pos];
        const std::size_t limit =
            var.type == variable_type::edge ? g.num_edges() : g.num_vertices();
        if (var.id >= limit)
            throw std::invalid_argument(
                "estimate_max_boundary_size: variable id " +
                std::to_string(var.id) + " out of range");
        if (var.type == variable_type::edge && edge_pos[var.id] == absent)
            edge_pos[var.id] = pos;
    }

    std::vector<std::size_t> total(g.num_vertices(), 0), done(g.num_vertices(), 0);
    for (std::size_t v = 0; v < g.num_vertices(); ++v)
        for (std::size_t eid : g.get_incident_edges(v))
            total[v] += (edge_pos[eid] != absent);

    auto on_boundary = [&](std::size_t v) { return done[v] > 0 && done[v] < total[v]; };

    std::size_t current = 0, max_boundary = 0;
    for (std::size_t level = 0; level < ordering.size(); ++level) {
        const variable& var = ordering[level];
        if (var.type == variable_type::edge && edge_pos[var.id] == level) {
            const edge& e = g.edges()[var.id];
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
