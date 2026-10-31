#include "common/validation.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace teddy::graphrel::detail {

namespace {

bool is_probability(double p) {
    // Written so that NaN is rejected as well.
    return p >= 0.0 && p <= 1.0;
}

[[noreturn]] void fail(const char* who, const std::string& what) {
    throw std::invalid_argument(std::string(who) + ": " + what);
}

} // namespace

const char* find_graph_problem(const graph& g) {
    if (g.num_vertices > graph::max_vertices)
        return "too many vertices (graphrel supports at most graph::max_vertices)";
    if (g.vertex_probs.size() != g.num_vertices || g.adj.size() != g.num_vertices)
        return "vertex_probs / adj size does not match num_vertices";
    if (g.edges.size() != g.num_edges)
        return "edges.size() does not match num_edges "
               "(graph(n, m) expects exactly m calls to add_edge)";

    for (double p : g.vertex_probs)
        if (!is_probability(p))
            return "vertex probability outside [0, 1]";

    std::size_t expected_adj_entries = 0;
    for (std::size_t i = 0; i < g.edges.size(); ++i) {
        const edge& e = g.edges[i];
        if (e.id != i)
            return "edge id does not match its index in edges";
        if (e.from >= g.num_vertices || e.to >= g.num_vertices)
            return "edge endpoint out of range";
        if (!is_probability(e.prob))
            return "edge probability outside [0, 1]";
        expected_adj_entries += (e.from == e.to) ? 1 : 2;
    }

    std::size_t adj_entries = 0;
    for (std::size_t v = 0; v < g.num_vertices; ++v) {
        for (std::size_t eid : g.adj[v]) {
            if (eid >= g.edges.size())
                return "adjacency list refers to a missing edge";
            const edge& e = g.edges[eid];
            if (e.from != v && e.to != v)
                return "adjacency list refers to a non-incident edge";
        }
        adj_entries += g.adj[v].size();
    }
    if (adj_entries != expected_adj_entries)
        return "adjacency lists are inconsistent with the edge list";

    if (g.terminals.empty())
        return "terminal set is empty";
    for (std::size_t i = 0; i < g.terminals.size(); ++i) {
        if (g.terminals[i] >= g.num_vertices)
            return "terminal vertex out of range";
        if (i > 0 && g.terminals[i - 1] >= g.terminals[i])
            return "terminals must be sorted and unique (use graph::set_terminals)";
    }
    return nullptr;
}

const graph& require_valid(const graph& g, const char* who) {
    if (const char* problem = find_graph_problem(g))
        fail(who, std::string("invalid graph: ") + problem);
    return g;
}

const std::vector<variable>& require_valid_ordering(
    const graph& g, const std::vector<variable>& ordering, const char* who) {
    constexpr std::size_t unseen = std::numeric_limits<std::size_t>::max();

    if (ordering.size() != g.num_vertices + g.num_edges)
        fail(who, "invalid ordering: it must contain every vertex and every edge exactly once "
                  "(expected " + std::to_string(g.num_vertices + g.num_edges) +
                  " variables, got " + std::to_string(ordering.size()) + ")");

    std::vector<std::size_t> vertex_pos(g.num_vertices, unseen);
    std::vector<char> edge_seen(g.num_edges, 0);

    for (std::size_t pos = 0; pos < ordering.size(); ++pos) {
        const variable& var = ordering[pos];
        if (var.type == variable_type::vertex) {
            if (var.id >= g.num_vertices)
                fail(who, "invalid ordering: vertex " + std::to_string(var.id) + " out of range");
            if (vertex_pos[var.id] != unseen)
                fail(who, "invalid ordering: vertex " + std::to_string(var.id) + " listed twice");
            vertex_pos[var.id] = pos;
        } else {
            if (var.id >= g.num_edges)
                fail(who, "invalid ordering: edge " + std::to_string(var.id) + " out of range");
            if (edge_seen[var.id])
                fail(who, "invalid ordering: edge " + std::to_string(var.id) + " listed twice");
            edge_seen[var.id] = 1;
            const edge& e = g.edges[var.id];
            if (vertex_pos[e.from] == unseen || vertex_pos[e.to] == unseen)
                fail(who, "invalid ordering: edge " + std::to_string(var.id) +
                          " appears before one of its endpoints (vertex priority is required)");
        }
    }
    // Sizes match and nothing was listed twice, so every variable appeared once.
    return ordering;
}

} // namespace teddy::graphrel::detail
