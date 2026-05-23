# Signal Tree

Signal Tree is a concurrent signal-selection data structure for fast, fair, idempotent wakeups when FIFO ordering is not the right abstraction.

First commit is just to make the code available with benchmarks

Complete READMEs are coming soon.

Repository:

https://github.com/buildingcpp/signal_tree

## Talks

- [CppCon 2024: Work Contracts - Rethinking Task Based Concurrency & Parallelism for Low Latency C++](https://www.youtube.com/watch?v=oj-_vpZNMVw)
- [CppCon 2025: Work Contracts in Action - Advancing High-performance, Low-latency Concurrency in C++](https://www.youtube.com/watch?v=5ghAa7B5bF0)
- [C++Now 2026: Signal Trees - A Deep Dive into a High-Performance Alternative to Queue-Based Task Scheduling](docs/Signal%20Trees_%20A%20Deep%20Dive%20into%20a%20High-Performance%20Alternative%20to%20Queue-Based%20Task%20Scheduling.pdf)

## The Benchmarks

## Throughput benchmark

The throughput benchmark measures the steady-state hot path: a thread selects a ready signal and immediately sets that same signal again. This is the best-case locality workload, and it answers the basic performance question: how fast can the structure repeatedly clear and re-set ready work under contention? The benchmark also reports signal-selection CV and thread-work CV, so it is not only measuring speed but also whether work is spread evenly across signals and worker threads. The current results show Signal Tree scaling far beyond the queue baselines while keeping both CV values very low.

## Adversarial throughput benchmark

The adversarial throughput benchmark measures cache-hostile signal migration. Instead of re-setting the same signal, each selected signal is replaced by a different physical signal: select X; set next[X]. This removes the hot-cache advantage of the static throughput test and better models workloads where readiness moves through a changing set of signals. Logical signals are mapped through a densest placement pass so the test does not accidentally measure bad ID layout. The current results show that Signal Tree still sustains high throughput and good fairness under migration.

## Service-distance benchmark

The service-distance benchmark measures fairness rather than raw speed. It tracks how long a scheduled signal waits before being serviced again, normalized by the active signal count. FIFO queues should stay near 1.0x, while Signal Tree is expected to have a wider non-FIFO envelope but no coverage failures. The important result is that Signal Tree services every scheduled signal, with a bounded service envelope, while some queue-like implementations may show either FIFO-flat behavior or, in the MoodyCamel case, visible coverage failure under this workload.


## Benchmark Results

- [Interactive benchmark dashboard](https://buildingcpp.github.io/signal_tree/)

# Signal Tree Size and Capacity

`signal_tree<N>` controls the depth and natural capacity of one physical Signal Tree. The default spelling `bcpp::signal_tree tree;` deduces `signal_tree<1>`, and `bcpp::signal_set signals{capacity};` deduces `signal_set<1>`, which is the balanced default. `signal_tree<0>` is a single 64-bit leaf node and is useful for small or heavily sharded workloads. Larger `N` values create larger single-tree fairness domains, reducing the need for sharding but increasing the amount of tree state involved in selection.

The measurements below are for one tree at its natural capacity. They do not include over-provisioning, `signal_set` vector overhead, benchmark storage, or user payloads. Unaligned nodes assume one 64-bit atomic word per node. Cache-line-aligned nodes assume one 64-byte aligned tree node.

| Tree Size | Capacity | Bits / Signal<br>[unaligned] | Bits / Signal<br>[cache-line aligned] |
|---:|---:|---:|---:|
| `signal_tree<0>` | 64 | 1.000 | 8.000 |
| `signal_tree<1>` | 512 | 1.125 | 9.000 |
| `signal_tree<2>` | 2,048 | 1.156 | 9.250 |
| `signal_tree<3>` | 8,192 | 1.164 | 9.312 |
| `signal_tree<4>` | 32,768 | 1.166 | 9.328 |
| `signal_tree<5>` | 131,072 | 1.167 | 9.332 |
| `signal_tree<6>` | 262,144 | 1.167 | 9.334 |
| `signal_tree<7>` | 524,288 | 1.167 | 9.335 |
| `signal_tree<8>` | 1,048,576 | 1.167 | 9.335 |


# Basic Usage

Signal Tree is a concurrent readiness set. It is useful when work is identified by a stable signal id, setting the same signal more than once should not enqueue duplicates, and consumers only need to select some ready signal rather than the oldest ready signal.

Signal Tree does not store payloads. The usual pattern is that a `signal_id` indexes work stored somewhere else.

## Header

```cpp
#include <include/signal_tree.h>
```

Use `signal_set` for the normal public API. It gives you a flat signal-id space and internally shards across one or more `signal_tree` instances.

## Create a signal set

```cpp
bcpp::signal_set signals{1024};
```

This uses the balanced default tree size, equivalent to `signal_set<1>`. The requested capacity is rounded up internally to a whole number of physical trees.

## Signal ids

A `signal_id` is the handle used to identify a signal.

```cpp
auto readyA = bcpp::signal_id{17};
auto readyB = bcpp::signal_id{42};
```

A signal id is only meaningful inside the signal set it belongs to. User code normally stores the id with the work object, contract, actor, mailbox, connection, or scheduler lane it represents.

## Set a signal

```cpp
signals.set(readyA);
signals.set(readyB);
```

Setting is idempotent. If a signal is already set, setting it again does not create another copy of the signal. This is the main semantic difference from a queue.

`set()` returns `true` only when the call changed the signal from clear to set.

```cpp
if (signals.set(readyA))
{
    // readyA transitioned from clear to set
}
```

## Select a signal

Selection clears one ready signal and returns its id.

```cpp
auto hint = bcpp::signal_id{0};

if (auto selected = signals.select(hint); selected.valid())
{
    // selected was ready and has now been cleared
    // hint has been updated for the next select call
}
```

The `hint` is caller-owned state. Keep one hint per selecting thread or per selector context. Reusing the hint improves locality and fairness behavior. Do not treat the hint as a selected signal; it is only a search position for the next call.

## Typical worker loop

```cpp
void worker(bcpp::signal_set & signals)
{
    auto hint = bcpp::signal_id{0};

    while (running)
    {
        auto signal = signals.select(hint);

        if (!signal.valid())
        {
            // No ready signal was found.
            // Sleep, yield, poll another source, or exit depending on your scheduler.
            continue;
        }

        service_work_for(signal);
    }
}
```

If selected work remains ready after being serviced, set the signal again:

```cpp
if (work_still_ready(signal))
{
    signals.set(signal);
}
```

## Mapping signals to work

A common pattern is to store work objects in a vector or table and use the signal id as the index.

```cpp
std::vector<execution_context> executionContext;
bcpp::signal_set signals{executionContext.size()};

void make_ready(std::size_t i)
{
    signals.set(bcpp::signal_id{i});
}

void service(bcpp::signal_id id)
{
    auto index = static_cast<bcpp::signal_id::value_type>(id);
    executionContext[index].run();
}
```

## Direct `signal_tree`

Most code should use `signal_set`. Direct `signal_tree` use is for lower-level or heavily tuned cases.

```cpp
bcpp::signal_tree tree;        // deduces signal_tree<1>
auto hint = bcpp::signal_id{0};

tree.set(bcpp::signal_id{7});

auto selected = tree.select(hint);
```

Unlike `signal_set`, direct `signal_tree` is a lower-level object with a fixed natural capacity. Bad ids are a precondition violation. Use `signal_set` unless you have a specific reason to manage tree capacity yourself.

## Tree size

The default spelling:

```cpp
bcpp::signal_tree tree;
bcpp::signal_set signals{capacity};
```

uses the balanced default tree size, equivalent to `<1>`.

Explicit tree sizes are available:

```cpp
bcpp::signal_set<0> tiny_or_heavily_sharded{capacity};
bcpp::signal_set<1> balanced_default{capacity};
bcpp::signal_set<4> larger_single_tree_domains{capacity};
```

`<0>` is a single 64-bit leaf tree. Larger values create larger physical trees and larger single-tree fairness domains.

## Selector policy

`select()` is policy-based. The default selector is the fairness selector.

```cpp
auto selected = signals.select(hint);
```

Other selectors can be supplied when appropriate:

```cpp
auto selected = signals.select<bcpp::densest_child_selector>(hint);
```

Most users should start with the default selector. Alternative selectors are intended for specialized placement, benchmarking, or future policy-specific scheduling behavior.
