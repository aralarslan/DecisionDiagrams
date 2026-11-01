#ifndef TEDDY_GRAPHREL_GRAPHREL_HPP
#define TEDDY_GRAPHREL_GRAPHREL_HPP

/**
 * @file graphrel.hpp
 * @brief Public umbrella header for libgraphrel
 *
 * Include this single header for access to the full graphrel API:
 *   - graph / edge types
 *   - read_graph / write_graph
 *   - build_diagram → reliability_diagram (BDD path, TeDDy analysis)
 *   - calculate_reliability (LBL scalar path)
 */

#include "teddy/graphrel/diagram.hpp"
#include "teddy/graphrel/graph.hpp"
#include "teddy/graphrel/graph_io.hpp"
#include "teddy/graphrel/reliability.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

#endif // TEDDY_GRAPHREL_GRAPHREL_HPP
