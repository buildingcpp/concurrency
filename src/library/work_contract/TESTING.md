# Work Contract test guide

The Work Contract suite is enabled by default when this repository is the
top-level project:

```bash
cmake -S . -B build -DWORK_CONTRACT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Active suites

| Suite | Primary coverage |
|---|---|
| `work_contract_test` | Multi-worker exact execution counts, slot recycling, stale handles, handles outliving groups, release and exception handlers. |
| `generation_test` | Generation preservation, deterministic ABA prevention, and high-churn concurrent stale-handle replay. |
| `api_test` | Type traits, callable constraints, ids, hashing, empty handles, capacity rounding, pool exhaustion, creation exception safety, initial scheduling, and basic execution results. |
| `lifecycle_test` | Schedule coalescing, persistent callable state, asynchronous capture destruction, release precedence/idempotence, move semantics, self-control, nested TLS context, and exception paths. |
| `concurrency_api_test` | Concurrent creation, schedule coalescence, schedule/release during execution, multi-worker release processing, group isolation, per-thread execution context, and explicit hints. |
| `blocking_test` | Indefinite waits, named and zero-timeout tries, bounded timeouts, wake-on-schedule, hint overloads, stop wakeups, invalidation, release, and multiple blocking workers. |
| `introduction_example_test` | Compiled executable form of the guide's first complete example, preventing introductory code drift. |

The concurrency tests use bounded waits so a broken synchronization path reports
a normal test failure instead of hanging indefinitely.

## Useful stress runs

Repeat every active suite to expose timing-sensitive failures:

```bash
ctest --test-dir build \
    --repeat until-fail:20 \
    --output-on-failure
```

For sanitizer builds, keep benchmarks off and run the same CTest command with the
compiler's AddressSanitizer/UndefinedBehaviorSanitizer or ThreadSanitizer flags.
ThreadSanitizer and AddressSanitizer should be separate builds.
