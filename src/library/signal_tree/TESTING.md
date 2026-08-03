# Signal Tree test guide

The Signal Tree suite is enabled by default when this repository is the
top-level project:

```bash
cmake -S . -B build -DSIGNAL_TREE_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Active suites

| Suite | Primary coverage |
|---|---|
| `signal_tree_test` | Tree layout, traits, selection, setting, and compile-time structure. |
| `signal_set_test` | Sharding, hint traversal, capacity, wraparound, and invalid signals. |
| `blocking_signal_set_test` | Blocking selection, try selection, timeouts, stop, and concurrent wakeups. |
| `selection_contract_test` | The complete tree/set selection contract across selectors, hints, and shards. |
| `signal_tree_concurrency_test` | Concurrent set/select behavior and publication under repeated contention. |

The concurrency tests use bounded waits so a broken synchronization path reports
a normal test failure instead of hanging indefinitely.

## Useful stress runs

```bash
ctest --test-dir build \
    --repeat until-fail:20 \
    --output-on-failure
```

Use separate ThreadSanitizer and AddressSanitizer/UndefinedBehaviorSanitizer
builds, with benchmarks disabled.
