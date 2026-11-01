# 🧸 TeDDy + libgraphrel: Exact $k$-terminal Network Reliability

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=c%2B%2B&logoColor=white)
![Research](https://img.shields.io/badge/Research-ENSEM%20%7C%20Université%20de%20Lorraine-orange)

This repository is an extended fork of [TeDDy](https://github.com/MichalMrena/DecisionDiagrams), adding **`libgraphrel`** — a module for exact $k$-terminal network reliability analysis.

This research was conducted under Professor Nicolae Brinzei ([ENSEM](https://ensem.univ-lorraine.fr/), [Université de Lorraine](https://www.univ-lorraine.fr/)).

> **Looking for the core TeDDy library?**  
> For the base library documentation, memory management, or core API, see the **[Original TeDDy README](./README_TeDDy.md)**.

---

## Contents

- [How it works](#how-it-works)
- [Prerequisites](#prerequisites)
- [How to install](#how-to-install)
- [How to use](#how-to-use)
- [The `.rel` input format](#the-rel-input-format-in-development)
- [Testing](#testing)
- [References and citation](#references-and-citation)

---

> **Project status:** This module is currently under active development as part of a research project at ENSEM, Université de Lorraine. API and file formats may still evolve.

---

## How it works

The core of `libgraphrel` is a graph-side decomposition engine. It operates on the probabilistic graph $G = (V, E, P_V, P_E)$ (represented by `teddy::graphrel::graph`) using Shannon decomposition and boundary-set partition isomorphism detection. From there, two execution paths are available.

### 1. LBL Calculator

An implementation of the Layer-by-Layer (LBL) recursive decomposition algorithm from Wu & Sun (2024). It traverses the network layer by layer and discards each layer once it has been processed.

`calculate_reliability(g)` returns the exact reliability $R(G, K)$ as a `double`. Only two layers are kept at a time, so memory stays bounded by the widest layer.

For more control, `analyze_reliability(g, options)` returns a `reliability_result` (`reliability`, `upper_bound`, `exact()`, `statistics`). `reliability_options` sets:

- `ordering`: a custom variable ordering (default: cut-width; `generate_variable_ordering(g, ordering_strategy::bfs)` reproduces Wu & Sun's setup);
- `threads`: threads for large layers (`1` by default, `0` for all cores). The result is bit-identical for any thread count;
- `prune_threshold`: drop branches with probability at or below it; the result is then an interval `[reliability, upper_bound]` guaranteed to contain $R$.

### 2. BDD Builder

Uses the same decomposition engine, but retains the full state space to construct a Binary Decision Diagram inside TeDDy.

`build_diagram(g)` returns a `teddy::graphrel::reliability_diagram` (`build_diagram(g, options)` takes a `diagram_options`: `ordering`, `threads`, `pool_size`). This struct owns a `bss_manager` and exposes `manager`, `diagram`, and `probs` as public members. You can use it with TeDDy's full analysis suite: logic derivatives (`dpld`), Structural Importance, Birnbaum Importance, and Minimal Cut Vectors (MCVs).

---

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| C++ compiler (GCC / Clang / MSVC) | C++20 | GCC ≥ 10, Clang ≥ 13, MSVC ≥ 19.31 |
| CMake | 3.19+ | |
| Boost.Test | -- | Tests only |

---

## How to install

Enable the `LIBTEDDY_BUILD_GRAPHREL` flag during CMake configuration.

```sh
git clone https://github.com/aral-ars/DecisionDiagrams.git
cd DecisionDiagrams

cmake -DCMAKE_BUILD_TYPE=Release \
      -DLIBTEDDY_BUILD_GRAPHREL=ON \
      -DLIBTEDDY_BUILD_EXAMPLES=ON \
      -S . -B build

cmake --build build -j4

# Optional: run the example to verify
./build/examples/graphrel-example
```

---

## How to use

The example below loads a network (or builds one in code), computes reliability using the LBL path, then builds a BDD and computes the structural importance of an edge.

```cpp
#include <teddy/graphrel/graphrel.hpp>
#include <libteddy/details/dplds.hpp>
#include <iostream>

int main () {
    // Option 1: Load a network from a .rel file
    auto g = teddy::graphrel::read_graph("data/graphrel/bridge.rel");

    /*
    // Option 2: Build the same bridge network in code
    teddy::graphrel::graph g(4, 5); // 4 vertices (perfect by default), room for 5 edges
    g.set_terminals({0, 3});        // k-terminal nodes
    g.set_all_vertex_probs(0.9);    // or g.set_vertex_prob(v, p) per vertex
    g.add_edge(0, 1, 0.9);
    g.add_edge(0, 2, 0.9);
    g.add_edge(1, 2, 0.9);
    g.add_edge(1, 3, 0.9);
    g.add_edge(2, 3, 0.9);
    */

    // Path 1: LBL Calculator — returns a scalar, discards intermediate layers.
    double r_lbl = teddy::graphrel::calculate_reliability(g);
    std::cout << "LBL Reliability: " << r_lbl << "\n";

    // The same with options: all cores (bit-identical result) and statistics.
    teddy::graphrel::reliability_options options;
    options.threads = 0;
    auto result = teddy::graphrel::analyze_reliability(g, options);
    std::cout << "Widest layer: " << result.statistics.max_layer_size << " states\n";

    // Path 2: BDD Builder — constructs a full diagram inside TeDDy
    auto bdd = teddy::graphrel::build_diagram(g);

    // Compute reliability from the diagram
    double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);

    // Compute structural importance of edge 0 (state change: 0 failed -> 1 working)
    auto dpld = bdd.manager.dpld(
        bdd.edge_change(0, 0, 1),
        teddy::dpld::basic(0, 1),
        bdd.diagram
    );
    double si = bdd.manager.structural_importance(dpld);

    std::cout << "Structural Importance of Edge 0: " << si << "\n";
}
```

---

## The `.rel` input format (in-development)

Networks are described in a DIMACS-style text format. Both edge and vertex failure probabilities are supported.

**Example (`bridge.rel`):**

```text
c Wu & Sun (2024) bridge network, Figure 1
c All components have reliability p = 0.9.
c Expected R(v0, v3) = 0.760078728
p reliability 4 5 0.9 0.9
t 0 3
e 0 1
e 0 2
e 1 2
e 1 3
e 2 3
```

| Token | Description |
|---|---|
| `c` | Comment |
| `p reliability <V> <E> [default_edge_prob [default_vertex_prob]]` | Problem definition |
| `t <terminal_0> <terminal_1> ...` | $k$-terminal nodes |
| `e <from> <to> [prob]` | Edge (uses default probability if omitted) |
| `v <vertex_id> <prob>` | Vertex probability override |

---

## Testing

The test suite checks results against the benchmark tables from Wu & Sun (2024). You will need [Boost.Test](https://www.boost.org/doc/libs/1_82_0/libs/test/doc/html/index.html) installed.

```sh
cmake -DCMAKE_BUILD_TYPE=Release \
      -DLIBTEDDY_BUILD_GRAPHREL=ON \
      -DLIBTEDDY_BUILD_TESTS=ON \
      -S . -B build

cmake --build build -j4

ctest --test-dir build -R teddy-test-graphrel -V
```

---

## References and citation

**Algorithm references:**
- [Wu & Sun (2024)](https://doi.org/10.1016/j.ress.2024.109968). *A novel layer-by-layer recursive decomposition algorithm for calculation of network reliability.*
- [Carlier & Lucet (1996)](https://doi.org/10.1016/0166-218X(95)00032-M). *A decomposition algorithm for network reliability evaluation.*
- Imai, H., Sekine, K., & Imai, K. (1999). *Computational Investigations of All-Terminal Network Reliability via BDDs.*

**Core library citation:**  
If you use this software in your research, please cite the original TeDDy paper:

```bibtex
@article{Mrena_SWX_2024,
  title   = {TeDDy: Templated decision diagram library},
  author  = {Michal Mrena and Miroslav Kvassay and Elena Zaitseva},
  year    = 2024,
  journal = {SoftwareX},
  volume  = 26,
  pages   = 101715,
  doi     = {https://doi.org/10.1016/j.softx.2024.101715}
}
```
