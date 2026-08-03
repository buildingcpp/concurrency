# Building C++ Concurrency

The `bcpp::concurrency` namespace contains two related C++20 libraries. Signal
Tree provides concurrent signal selection; Work Contract uses it to schedule
persistent activities on application-owned worker threads.

| Library | Header | CMake target | Documentation |
|---|---|---|---|
| Signal Tree | `<library/signal_tree.h>` | `bcpp::signal_tree` | [Guide](src/library/signal_tree/README.md), [testing](src/library/signal_tree/TESTING.md), [future work](src/library/signal_tree/FUTURE_WORK.md) |
| Work Contract | `<library/work_contract.h>` | `bcpp::work_contract` | [Introduction](src/library/work_contract/INTRODUCTION.md), [concurrency model](src/library/work_contract/CONCURRENCY.md), [testing](src/library/work_contract/TESTING.md) |
| Complete namespace | `<library/concurrency.h>` | `bcpp::concurrency` | Includes both libraries |

`bcpp::work_contract` links `bcpp::signal_tree` publicly, so consumers of Work
Contract receive its Signal Tree dependency automatically. `bcpp::concurrency`
links the complete target graph.

## Build and test

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Benchmarks are opt-in through `SIGNAL_TREE_BUILD_BENCHMARK` and
`WORK_CONTRACT_BUILD_BENCHMARK`.
