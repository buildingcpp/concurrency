# Building C++ Concurrency

This repository contains two related C++20 concurrency libraries:

- **Signal Tree** is a concurrent readiness set for fast, idempotent signal
  selection when FIFO ordering is unnecessary.
- **Work Contract** stores persistent activities, schedules them through Signal
  Tree, and lets application-owned workers execute them.

The libraries share the `bcpp::concurrency` namespace but remain independently
usable CMake targets.

| Use | Header | CMake target |
|---|---|---|
| Signal Tree only | `<library/signal_tree.h>` | `bcpp::signal_tree` |
| Work Contract and its automatic Signal Tree dependency | `<library/work_contract.h>` | `bcpp::work_contract` |
| Complete repository API | `<library/concurrency.h>` | `bcpp::concurrency` |

Start with the [`bcpp::concurrency` documentation](CONCURRENCY.md), which links
the guides, concurrency contracts, test coverage, and future work for each
library.

## Build and test

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Benchmarks are opt-in through `SIGNAL_TREE_BUILD_BENCHMARK` and
`WORK_CONTRACT_BUILD_BENCHMARK` so ordinary builds and downstream consumers do
not acquire benchmark-only dependencies.

The project is licensed under the [MIT License](LICENSE).
