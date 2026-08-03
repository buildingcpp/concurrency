# Work Contract

Work Contract is a C++20 library for persistent, serialized activities that are
signaled cheaply and executed by application-owned workers. It stores each
callable once, gives it a stable identity, and coalesces repeated scheduling
requests while work is already pending or executing.

Start with [the practical introduction](INTRODUCTION.md). The
[concurrency contract](CONCURRENCY.md) documents the synchronization and
lifecycle rules, and [the test guide](TESTING.md) describes the active
validation suites.

WC uses [Signal Tree](https://github.com/buildingcpp/signal_tree) for efficient
signal selection.

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The project is licensed under the [MIT License](LICENSE).
