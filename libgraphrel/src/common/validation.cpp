#include "common/validation.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace teddy::graphrel::detail {

namespace {

[[noreturn]] void fail(const char* who, const std::string& what) {
    throw std::invalid_argument(std::string(who) + ": " + what);
}

} // namespace

const char* find_graph_problem(const graph& g) {
    // Sizes, endpoints, probabilities, adjacency and terminal order are
    // invariants of graph; only the terminal set can still be missing.
    if (g.terminals().empty())
        return "terminal set is empty (use graph::set_terminals)";
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

    if (ordering.size() != g.num_vertices() + g.num_edges())
        fail(who, "invalid ordering: it must contain every vertex and every edge exactly once "
                  "(expected " + std::to_string(g.num_vertices() + g.num_edges()) +
                  " variables, got " + std::to_string(ordering.size()) + ")");

    std::vector<std::size_t> vertex_pos(g.num_vertices(), unseen);
    std::vector<char> edge_seen(g.num_edges(), 0);

    for (std::size_t pos = 0; pos < ordering.size(); ++pos) {
        const variable& var = ordering[pos];
        if (var.type == variable_type::vertex) {
            if (var.id >= g.num_vertices())
                fail(who, "invalid ordering: vertex " + std::to_string(var.id) + " out of range");
            if (vertex_pos[var.id] != unseen)
                fail(who, "invalid ordering: vertex " + std::to_string(var.id) + " listed twice");
            vertex_pos[var.id] = pos;
        } else {
            if (var.id >= g.num_edges())
                fail(who, "invalid ordering: edge " + std::to_string(var.id) + " out of range");
            if (edge_seen[var.id])
                fail(who, "invalid ordering: edge " + std::to_string(var.id) + " listed twice");
            edge_seen[var.id] = 1;
            const edge& e = g.edges()[var.id];
            if (vertex_pos[e.from] == unseen || vertex_pos[e.to] == unseen)
                fail(who, "invalid ordering: edge " + std::to_string(var.id) +
                          " appears before one of its endpoints (vertex priority is required)");
        }
    }
    // Sizes match and nothing was listed twice, so every variable appeared once.
    return ordering;
}

} // namespace teddy::graphrel::detail
