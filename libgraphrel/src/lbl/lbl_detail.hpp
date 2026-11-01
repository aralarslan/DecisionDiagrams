// Private implementation header of the layer-by-layer calculator — not
// installed. Used by lbl_calculator.cpp and tests.

#ifndef GRAPHREL_SRC_LBL_DETAIL_HPP
#define GRAPHREL_SRC_LBL_DETAIL_HPP

#include <cstddef>

#include "teddy/graphrel/reliability.hpp"

namespace teddy::graphrel::detail
{

/// Testing hook: parents per summation block, and the smallest layer that is
/// expanded in parallel. A different block size can change the last bits of
/// the result (it regroups floating-point sums).
struct parallel_grain
{
    std::size_t block_size         = 4096;
    std::size_t min_parallel_layer = 16384;
};

/// analyze_reliability() with an explicit parallel grain.
reliability_result analyze_reliability (
    graph const& g,
    reliability_options const& options,
    parallel_grain const& grain
);

} // namespace teddy::graphrel::detail

#endif // GRAPHREL_SRC_LBL_DETAIL_HPP
