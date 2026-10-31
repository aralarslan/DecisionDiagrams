/**
 * @file graphrel_checks.test.cpp
 * @brief Brute-force fuzzing, precondition, I/O and paper-reproduction tests
 *
 * - graphrel_fuzz_test:          LBL and BDD vs exhaustive enumeration on
 *                                random small graphs, several input modes
 *                                and orderings.
 * - graphrel_preconditions_test: invalid graphs / orderings are rejected,
 *                                |K| = 1 semantics, regressions from the
 *                                libgraphrel audit.
 * - graphrel_io_test:            strict parsing and exact round-trips.
 * - graphrel_paper_test:         values from Wu & Sun (2024) Table 2 and
 *                                Carlier & Lucet (1996) Algorithms 1-2.
 */

#include <boost/test/unit_test.hpp>
#include <boost/test/tools/floating_point_comparison.hpp>

#include <teddy/graphrel/graphrel.hpp>
#include "common/bell_cache.hpp"
#include "common/partition.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace teddy::graphrel;

// ── helpers ──────────────────────────────────────────────────────────────────

/// Exact K-terminal reliability by enumerating all 2^(V+E) states.
double brute_force(const graph& g) {
    const std::size_t V = g.num_vertices, E = g.edges.size();
    double total = 0.0;
    std::vector<std::size_t> parent(V);
    for (std::uint64_t m = 0; m < (std::uint64_t{1} << (V + E)); ++m) {
        double p = 1.0;
        for (std::size_t i = 0; i < V; ++i)
            p *= (m >> i & 1) ? g.vertex_probs[i] : 1.0 - g.vertex_probs[i];
        for (std::size_t j = 0; j < E; ++j)
            p *= (m >> (V + j) & 1) ? g.edges[j].prob : 1.0 - g.edges[j].prob;
        if (p == 0.0)
            continue;
        bool ok = true;
        for (std::size_t t : g.terminals)
            ok = ok && (m >> t & 1);
        if (!ok)
            continue;
        std::iota(parent.begin(), parent.end(), std::size_t{0});
        auto find = [&](std::size_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        for (std::size_t j = 0; j < E; ++j) {
            const edge& e = g.edges[j];
            if ((m >> (V + j) & 1) && (m >> e.from & 1) && (m >> e.to & 1))
                parent[find(e.from)] = find(e.to);
        }
        const std::size_t root = find(g.terminals.front());
        for (std::size_t t : g.terminals)
            ok = ok && find(t) == root;
        if (ok)
            total += p;
    }
    return total;
}

double bdd_reliability(const graph& g) {
    auto d = build_diagram(g);
    return d.manager.calculate_probability(d.probs, d.diagram);
}

std::string dump(const graph& g) {
    std::ostringstream os;
    write_graph(os, g);
    return os.str();
}

enum class fuzz_mode {
    paper,          ///< Wu & Sun assumptions: connected simple graph, |K| >= 2, p in (0,1)
    paper_perfect,  ///< as paper, but some edges have p = 1
    any_k2,         ///< anything (disconnected, loops, multi-edges, p in {0,1}), |K| >= 2
    any             ///< anything, including |K| = 1
};

graph random_graph(std::mt19937_64& rng, fuzz_mode mode) {
    const bool paper = mode == fuzz_mode::paper || mode == fuzz_mode::paper_perfect;
    auto rand_p = [&] { return 0.1 + 0.8 * static_cast<double>(rng() % 1000) / 1000.0; };
    auto pick = [&] { const double ps[] = {0.0, 0.3, 0.5, 0.9, 1.0}; return ps[rng() % 5]; };

    const std::size_t V = paper ? 3 + rng() % 5 : 2 + rng() % 6;
    std::size_t E = 1 + rng() % 9;
    std::vector<std::pair<std::size_t, std::size_t>> ends;
    if (paper) {
        for (std::size_t v = 1; v < V; ++v)
            ends.emplace_back(rng() % v, v);  // spanning tree: connected
        E = std::min(std::max(E, V - 1), V * (V - 1) / 2);
        while (ends.size() < E) {
            std::size_t a = rng() % V, b = rng() % V;
            if (a == b) continue;
            if (a > b) std::swap(a, b);
            if (std::find(ends.begin(), ends.end(), std::make_pair(a, b)) == ends.end())
                ends.emplace_back(a, b);
        }
    } else {
        for (std::size_t j = 0; j < E; ++j)
            ends.emplace_back(rng() % V, rng() % V);
    }

    graph g(V, ends.size());
    const bool perfect_edges = mode == fuzz_mode::paper_perfect ||
                               (!paper && rng() % 3 == 0);
    for (auto [a, b] : ends)
        g.add_edge(a, b, perfect_edges && rng() % 2 ? 1.0 : rand_p());
    if (paper || rng() % 2)
        for (double& p : g.vertex_probs)
            p = paper ? rand_p() : (rng() % 3 ? pick() : 1.0);

    const std::size_t min_k = mode == fuzz_mode::any ? 1 : 2;
    const std::size_t k = min_k + rng() % std::min<std::size_t>(V - min_k + 1, 3);
    std::vector<std::size_t> ids(V);
    std::iota(ids.begin(), ids.end(), std::size_t{0});
    std::shuffle(ids.begin(), ids.end(), rng);
    g.set_terminals({ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(k)});
    return g;
}

/// Random ordering that respects vertex priority but is otherwise arbitrary.
std::vector<variable> random_vertex_priority_ordering(const graph& g, std::mt19937_64& rng) {
    std::vector<std::size_t> verts(g.num_vertices);
    std::iota(verts.begin(), verts.end(), std::size_t{0});
    std::shuffle(verts.begin(), verts.end(), rng);
    std::vector<char> placed(g.num_vertices, 0), done(g.edges.size(), 0);
    std::vector<std::size_t> ready;
    std::vector<variable> ord;
    std::size_t next = 0;
    while (ord.size() < g.num_vertices + g.edges.size()) {
        if (next < verts.size() && (ready.empty() || rng() % 2)) {
            const std::size_t v = verts[next++];
            placed[v] = 1;
            ord.emplace_back(variable_type::vertex, v);
            for (std::size_t e = 0; e < g.edges.size(); ++e)
                if (!done[e] && placed[g.edges[e].from] && placed[g.edges[e].to]) {
                    done[e] = 1;
                    ready.push_back(e);
                }
        } else {
            const std::size_t i = rng() % ready.size();
            ord.emplace_back(variable_type::edge, ready[i]);
            ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    return ord;
}

graph mesh(std::size_t rows, std::size_t cols, double p) {
    graph g(rows * cols, rows * (cols - 1) + cols * (rows - 1));
    for (std::size_t i = 0; i < rows; ++i)
        for (std::size_t j = 0; j < cols; ++j) {
            if (j + 1 < cols) g.add_edge(i * cols + j, i * cols + j + 1, p);
            if (i + 1 < rows) g.add_edge(i * cols + j, (i + 1) * cols + j, p);
        }
    for (double& v : g.vertex_probs) v = p;
    g.set_terminals({0, rows * cols - 1});
    return g;
}

constexpr double exact_tol = 1e-9;

} // namespace

// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(graphrel_fuzz_test)

namespace {

void run_fuzz(fuzz_mode mode, int iterations, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    int lbl_bad = 0, bdd_bad = 0, dfs_bad = 0, random_bad = 0;
    for (int it = 0; it < iterations; ++it) {
        const graph g = random_graph(rng, mode);
        const double exact = brute_force(g);
        auto off = [&](double r) { return std::fabs(r - exact) > exact_tol; };

        const double lbl = calculate_reliability(g);
        const double bdd = bdd_reliability(g);
        const double dfs = calculate_reliability(
            g, generate_variable_ordering(g, ordering_strategy::dfs));
        const double rnd = calculate_reliability(g, random_vertex_priority_ordering(g, rng));

        lbl_bad += off(lbl);
        bdd_bad += off(bdd);
        dfs_bad += off(dfs);
        random_bad += off(rnd);
        if (off(lbl) || off(bdd) || off(dfs) || off(rnd)) {
            BOOST_TEST_MESSAGE("mismatch at iteration " << it << ": exact=" << exact
                               << " lbl=" << lbl << " bdd=" << bdd << " dfs=" << dfs
                               << " random=" << rnd << "\n" << dump(g));
        }
    }
    BOOST_TEST(lbl_bad == 0);
    BOOST_TEST(bdd_bad == 0);
    BOOST_TEST(dfs_bad == 0);
    BOOST_TEST(random_bad == 0);
}

} // namespace

BOOST_AUTO_TEST_CASE(fuzz_paper_assumptions)       { run_fuzz(fuzz_mode::paper, 1500, 1); }
BOOST_AUTO_TEST_CASE(fuzz_paper_with_perfect_edges) { run_fuzz(fuzz_mode::paper_perfect, 1500, 2); }
BOOST_AUTO_TEST_CASE(fuzz_anything_k2)              { run_fuzz(fuzz_mode::any_k2, 1500, 3); }
BOOST_AUTO_TEST_CASE(fuzz_anything)                 { run_fuzz(fuzz_mode::any, 1500, 4); }

BOOST_AUTO_TEST_SUITE_END()

// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(graphrel_preconditions_test)

BOOST_AUTO_TEST_CASE(perfect_edges_through_failed_vertex) {
    // Audit 1.1: BDD contracted p=1 edges incident to a failed vertex.
    graph g(3, 3);
    g.vertex_probs[1] = 0.5;
    g.add_edge(0, 1, 1.0);
    g.add_edge(1, 2, 1.0);
    g.add_edge(0, 2, 0.5);
    g.set_terminals({0, 2});
    BOOST_TEST(calculate_reliability(g) == 0.75, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(bdd_reliability(g) == 0.75, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(single_terminal_is_its_own_reliability) {
    // Audit 1.2: |K| = 1 gives R = p(terminal) on both paths.
    graph g(2, 1);
    g.vertex_probs[0] = 0.3;
    g.add_edge(0, 1, 0.5);
    g.set_terminals({0});
    BOOST_TEST(calculate_reliability(g) == 0.3, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(bdd_reliability(g) == 0.3, boost::test_tools::tolerance(1e-12));
    // Any valid ordering, even one that does not start at the terminal.
    std::vector<variable> ord{{variable_type::vertex, 1}, {variable_type::vertex, 0},
                              {variable_type::edge, 0}};
    BOOST_TEST(calculate_reliability(g, ord) == 0.3, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(unsorted_terminals_rejected) {
    // Audit 1.3: unsorted terminals used to give 0.855 instead of 0.405.
    graph g(3, 2);
    g.vertex_probs[0] = 0.5;
    g.add_edge(0, 1, 0.9);
    g.add_edge(1, 2, 0.9);
    g.terminals = {2, 0};
    BOOST_TEST(!g.validate());
    BOOST_CHECK_THROW(calculate_reliability(g), std::invalid_argument);
    BOOST_CHECK_THROW(build_diagram(g), std::invalid_argument);

    g.set_terminals({2, 0, 2});
    BOOST_TEST((g.terminals == std::vector<std::size_t>{0, 2}));
    BOOST_TEST(calculate_reliability(g) == 0.405, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(bdd_reliability(g) == 0.405, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(empty_terminals_rejected) {
    graph g(2, 1);
    g.add_edge(0, 1, 0.9);
    BOOST_TEST(!g.validate());
    BOOST_CHECK_THROW(calculate_reliability(g), std::invalid_argument);
    BOOST_CHECK_THROW(build_diagram(g), std::invalid_argument);
    BOOST_CHECK_THROW(g.set_terminals({}), std::invalid_argument);
    BOOST_CHECK_THROW(g.set_terminals({5}), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(nan_probabilities_rejected) {
    graph g(2, 1);
    BOOST_CHECK_THROW(g.add_edge(0, 1, std::nan("")), std::invalid_argument);
    g.add_edge(0, 1, 0.5);
    g.vertex_probs[0] = std::nan("");
    g.set_terminals({0, 1});
    BOOST_TEST(!g.validate());
}

BOOST_AUTO_TEST_CASE(edge_count_mismatch_rejected) {
    graph g(3, 0);   // declares zero edges ...
    g.add_edge(0, 1, 0.9);  // ... but adds one
    g.set_terminals({0, 1});
    BOOST_TEST(!g.validate());
    BOOST_CHECK_THROW(calculate_reliability(g), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(invalid_orderings_rejected) {
    // Audit 1.4: these used to return 0.81 and 0.0 instead of 0.405.
    graph g(3, 2);
    g.vertex_probs[1] = 0.5;
    g.add_edge(0, 1, 0.9);
    g.add_edge(1, 2, 0.9);
    g.set_terminals({0, 2});
    using vt = variable_type;

    std::vector<variable> edges_first{{vt::edge, 0}, {vt::edge, 1}, {vt::vertex, 0},
                                      {vt::vertex, 1}, {vt::vertex, 2}};
    std::vector<variable> missing{{vt::vertex, 0}, {vt::edge, 0}};
    std::vector<variable> duplicate{{vt::vertex, 0}, {vt::vertex, 1}, {vt::vertex, 1},
                                    {vt::edge, 0}, {vt::edge, 1}};
    std::vector<variable> out_of_range{{vt::vertex, 0}, {vt::vertex, 1}, {vt::vertex, 7},
                                       {vt::edge, 0}, {vt::edge, 1}};
    BOOST_CHECK_THROW(calculate_reliability(g, edges_first), std::invalid_argument);
    BOOST_CHECK_THROW(calculate_reliability(g, missing), std::invalid_argument);
    BOOST_CHECK_THROW(calculate_reliability(g, duplicate), std::invalid_argument);
    BOOST_CHECK_THROW(calculate_reliability(g, out_of_range), std::invalid_argument);

    std::vector<variable> ok{{vt::vertex, 2}, {vt::vertex, 1}, {vt::edge, 1},
                             {vt::vertex, 0}, {vt::edge, 0}};
    BOOST_TEST(calculate_reliability(g, ok) == 0.405, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(small_unreliability_is_exact) {
    // Audit 1.5: the 1e-15 cutoff gave a 1.2e-6 relative error in 1-R here.
    const graph g = [] {
        graph m = mesh(5, 5, 0.9999);
        std::fill(m.vertex_probs.begin(), m.vertex_probs.end(), 1.0);
        return m;
    }();
    const double u_lbl = 1.0 - calculate_reliability(g);
    const double u_bdd = 1.0 - bdd_reliability(g);
    BOOST_TEST(std::fabs(u_lbl - u_bdd) / u_bdd < 1e-7);
}

BOOST_AUTO_TEST_SUITE_END()

// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(graphrel_io_test)

namespace {
graph parse(const std::string& text) {
    std::istringstream in(text);
    return read_graph(in);
}
} // namespace

BOOST_AUTO_TEST_CASE(valid_file_parses) {
    graph g = parse("c comment\n"
                    "p reliability 3 2 0.9 0.8\n"
                    "t 2 0\n"
                    "e 0 1\n"
                    "e 1 2 0.5\n"
                    "v 1 0.7\n");
    BOOST_TEST(g.num_vertices == 3u);
    BOOST_TEST(g.edges[0].prob == 0.9);
    BOOST_TEST(g.edges[1].prob == 0.5);
    BOOST_TEST(g.vertex_probs[0] == 0.8);
    BOOST_TEST(g.vertex_probs[1] == 0.7);
    BOOST_TEST((g.terminals == std::vector<std::size_t>{0, 2}));
}

BOOST_AUTO_TEST_CASE(malformed_input_rejected) {
    const char* bad[] = {
        "p reliability 2 1\nt 0 1\ne 0 1 O.9\n",        // letter O: used to parse as 0
        "p reliability 2 1 0.9x\nt 0 1\ne 0 1\n",       // trailing junk on a number
        "p reliability 2 1\nt 0 1\ne 0 1 0.5 extra\n",  // extra token
        "p reliability -3 1\nt 0\ne 0 0\n",             // negative count
        "p reliability 2 1\nt 0 1\ne 0 1 1.5\n",        // out of range
        "p reliability 2 1\nt 0 1\ne 0 1 nan\n",        // not a number
        "p reliability 2 1\nt\ne 0 1\n",                // empty terminal set
        "p reliability 2 1\nt 0 1\nedge 0 1\n",         // unknown line type
        "p reliability 2 2\nt 0 1\ne 0 1\n",            // edge count mismatch
        "p reliability 70000 0\nt 0\n",                 // too many vertices
    };
    for (const char* text : bad)
        BOOST_CHECK_THROW(parse(text), ParseError);
}

BOOST_AUTO_TEST_CASE(round_trip_is_exact) {
    graph g(3, 2);
    g.add_edge(0, 1, 0.1 + 0.2);       // 0.30000000000000004
    g.add_edge(1, 2, 1.0 / 3.0);
    g.vertex_probs[1] = 2.0 / 3.0;
    g.set_terminals({0, 2});
    std::stringstream ss;
    write_graph(ss, g);
    const graph h = read_graph(ss);
    BOOST_TEST(h.edges[0].prob == g.edges[0].prob);
    BOOST_TEST(h.edges[1].prob == g.edges[1].prob);
    BOOST_TEST(h.vertex_probs[1] == g.vertex_probs[1]);
}

BOOST_AUTO_TEST_SUITE_END()

// ═════════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(graphrel_paper_test)

// Wu & Sun (2024), Table 2: all edges and vertices 0.9, terminals at maximum
// distance. N6-N9 are meshes (identified from |V| and |E| in Table 1).
BOOST_AUTO_TEST_CASE(wu_sun_table2_meshes) {
    struct row { std::size_t r, c; double expected; };
    for (row t : {row{20, 2, 0.230160},   // N9
                  row{10, 3, 0.609632},   // N6
                  row{12, 3, 0.579399},   // N7
                  row{5, 5, 0.711988}}) { // N8
        const graph g = mesh(t.r, t.c, 0.9);
        BOOST_TEST(calculate_reliability(g) == t.expected, boost::test_tools::tolerance(1e-6));
        BOOST_TEST(bdd_reliability(g) == t.expected, boost::test_tools::tolerance(1e-6));
    }
}

BOOST_AUTO_TEST_CASE(wu_sun_table2_complete_graph_k12) {
    // N27: complete graph on 12 vertices, R = 0.810000. The exact run takes
    // ~100 s and 1.2 GB (1.46 M states per layer), so this uses the pruned
    // variant and checks that its rigorous interval is tight and contains the
    // published value.
    const std::size_t n = 12;
    graph g(n, n * (n - 1) / 2);
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = a + 1; b < n; ++b)
            g.add_edge(a, b, 0.9);
    std::fill(g.vertex_probs.begin(), g.vertex_probs.end(), 0.9);
    g.set_terminals({0, n - 1});
    const reliability_bounds r = calculate_reliability_bounds(g, 1e-15);
    BOOST_TEST(r.lower <= r.upper);
    BOOST_TEST(r.upper - r.lower < 1e-9);
    BOOST_TEST(r.lower == 0.81, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(pruned_bounds_contain_exact_value) {
    std::mt19937_64 rng(5);
    for (int it = 0; it < 300; ++it) {
        const graph g = random_graph(rng, fuzz_mode::any_k2);
        const double exact = brute_force(g);
        const reliability_bounds b0 = calculate_reliability_bounds(g, 0.0);
        BOOST_TEST(b0.lower == exact, boost::test_tools::tolerance(exact_tol));
        BOOST_TEST(b0.upper == b0.lower);
        const reliability_bounds b = calculate_reliability_bounds(g, 0.05);  // aggressive
        BOOST_TEST(b.lower <= exact + exact_tol);
        BOOST_TEST(b.upper >= exact - exact_tol);
    }
    graph g(2, 1);
    g.add_edge(0, 1, 0.5);
    g.set_terminals({0, 1});
    BOOST_CHECK_THROW(calculate_reliability_bounds(g, -1.0), std::invalid_argument);
    BOOST_CHECK_THROW(calculate_reliability_bounds(g, std::nan("")), std::invalid_argument);
}

// Carlier & Lucet (1996) §4.7, Algorithm 2 transcribed literally, compared
// with partition_to_number over every partition of up to 8 elements.
BOOST_AUTO_TEST_CASE(carlier_lucet_class_numbering) {
    const bell_cache cache(12);
    auto cl_number = [&](const std::vector<int>& part) {
        std::uint64_t num = 1;
        int j = 1;
        for (std::size_t i = 2; i <= part.size(); ++i) {
            if (part[i - 1] == j + 1) {
                ++j;
                num += cache.stirling(i - 1, static_cast<std::size_t>(j)) * static_cast<std::uint64_t>(j);
            } else {
                num = (num - 1) * static_cast<std::uint64_t>(j) + static_cast<std::uint64_t>(part[i - 1]);
            }
        }
        for (int h = 1; h < j; ++h)
            num += cache.stirling(part.size(), static_cast<std::size_t>(h));
        return num;
    };

    std::size_t bad = 0;
    for (std::size_t F = 1; F <= 8; ++F) {
        std::vector<int> part(F, 1);
        std::vector<char> seen(cache.bell(F) + 1, 0);
        std::function<void(std::size_t, int)> rec = [&](std::size_t i, int max_block) {
            if (i == F) {
                const std::uint64_t n = partition_to_number(part, max_block, cache);
                if (n != cl_number(part) || n < 1 || n > cache.bell(F) || seen[n] ||
                    number_to_partition(n, F, cache) != part)
                    ++bad;
                else
                    seen[n] = 1;
                return;
            }
            for (int b = 1; b <= max_block + 1; ++b) {
                part[i] = b;
                rec(i + 1, std::max(max_block, b));
            }
        };
        rec(1, 1);
    }
    BOOST_TEST(bad == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
