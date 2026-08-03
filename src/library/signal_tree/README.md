# Signal Tree

Signal Tree is a concurrent signal-selection data structure for fast, fair, idempotent wakeups when FIFO ordering is not the right abstraction.

First commit is just to make the code available with benchmarks

Complete READMEs are coming soon.

Repository:

https://github.com/buildingcpp/concurrency

## Talks

- [CppCon 2024: Work Contracts - Rethinking Task Based Concurrency & Parallelism for Low Latency C++](https://www.youtube.com/watch?v=oj-_vpZNMVw)
- [CppCon 2025: Work Contracts in Action - Advancing High-performance, Low-latency Concurrency in C++](https://www.youtube.com/watch?v=5ghAa7B5bF0)
- [C++Now 2026: Signal Trees - A Deep Dive into a High-Performance Alternative to Queue-Based Task Scheduling](../../../docs/signal_tree/Signal%20Trees_%20A%20Deep%20Dive%20into%20a%20High-Performance%20Alternative%20to%20Queue-Based%20Task%20Scheduling.pdf)

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

`signal_tree<N>` controls the depth and natural capacity of one physical Signal Tree. The default spelling `bcpp::concurrency::signal_tree tree;` deduces `signal_tree<1>`, and `bcpp::concurrency::signal_set signals{capacity};` deduces `signal_set<1>`, which is the balanced default. `signal_tree<0>` is a single 64-bit leaf node and is useful for small or heavily sharded workloads. Larger `N` values create larger single-tree fairness domains, reducing the need for sharding but increasing the amount of tree state involved in selection.

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
#include <library/signal_tree.h>
```

Use `signal_set` for the normal public API. It gives you a flat signal-id space and internally shards across one or more `signal_tree` instances.

## Create a signal set

```cpp
bcpp::concurrency::signal_set signals{1024};
```

This uses the balanced default tree size, equivalent to `signal_set<1>`. The requested capacity is rounded up internally to a whole number of physical trees.

## Signal ids

A `signal_id` is the handle used to identify a signal.

```cpp
auto readyA = bcpp::concurrency::signal_id{17};
auto readyB = bcpp::concurrency::signal_id{42};
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
auto hint = bcpp::concurrency::signal_id{0};

if (auto selected = signals.select(hint); selected.valid())
{
    // selected was ready and has now been cleared
    // hint has been updated for the next select call
}
```

The `hint` is caller-owned state. Keep one hint per selecting thread or per selector context. Reusing the hint improves locality and fairness behavior. Do not treat the hint as a selected signal; it is only a search position for the next call.

## Selection contract

The fundamental selection guarantee is availability:

> If any signal remains raised in a `signal_tree` or `signal_set`, `select()` shall
> return a valid signal. It shall return an invalid signal only when no raised
> signal remains available for selection.

For a stable set of raised signals, a successful call clears and returns exactly
one signal. Repeated calls must drain every raised signal without duplication or
omission. An empty selection returns an invalid signal and invalidates the hint
for both `signal_tree` and `signal_set`.

With the default selector, traversal begins at the supplied hint. The hinted
signal is selected when it is raised; otherwise selection proceeds toward the
next available signal and wraps when necessary. An invalid or out-of-range hint
starts traversal from signal zero.

For `signal_tree<0>`, this is exact circular numeric ordering. With signals `3`
and `5` raised:

| Initial hint | Selection order |
|---:|---:|
| `2` or `3` | `3`, then `5` |
| `4` or `5` | `5`, then `3` |
| `62` | `3`, then `5` |

In the final case, selecting `3` updates the hint to `5`; the known next signal
must not be discarded during wraparound.

Larger trees use parent counters containing the sum of raised signals in each
child. Selection reserves a path by decrementing those counters while descending
the tree. After making that reservation, selection must consume a signal from the
reserved child to preserve the parent/child sum invariant. If the forward portion
of that child contains no raised signal, selection may move backward within the
child rather than abandon the reservation. This is the permitted exception to
strict forward traversal for `signal_tree<1>` and larger.

`signal_set` extends the same guarantee across every physical tree. Empty trees,
shard boundaries, and wraparound must not cause an invalid result while another
tree contains a raised signal. A depth-zero set retains exact circular numeric
ordering across its shards. Sets using larger trees retain the permitted backward
selection within a reserved child.

On success, the updated hint is the preferred starting position for the next
call. It is continuation state, not a promise that the hinted signal is currently
raised. Alternative selectors may replace the default hint-order preference, but
they do not weaken the availability, single-consumption, or parent/child sum
guarantees.

Concurrent setters may publish signals while selection is traversing, and other
selectors may consume them. No snapshot ordering is promised in that case. In
particular, a selector may return invalid if another selector consumes the final
raised signal before it can be claimed. A successfully reserved selection must
still complete without losing or duplicating a signal.

An optional strict-forward traversal is recorded as future work in
[FUTURE_WORK.md](FUTURE_WORK.md). It is not part of the current selection
contract.

## Blocking selection

Blocking selection is enabled at compile time:

```cpp
using blocking_signal_set =
        bcpp::concurrency::signal_set<1, bcpp::synchronization_mode::blocking>;

blocking_signal_set signals{1024};
auto hint = bcpp::concurrency::signal_id{0};
auto selected = signals.select(hint, std::chrono::milliseconds{10});
```

The duration overload first performs an ordinary immediate selection. If no
signal is available, it waits until a signal becomes available, the timeout
expires, or `stop()` interrupts the wait. A timeout of zero performs exactly one
immediate selection attempt and never waits. Ordinary `select(hint)` remains
immediate even on a blocking signal set or tree.

### Stop contract

`stop()` is a terminal state for waiting, not for signaling or selection. It
wakes calls currently blocked in timed selection. A blocked call that observes
the stopped state returns an invalid signal. Future timed selections still make
their initial immediate selection attempt, but return invalid instead of waiting
when that attempt finds no signal.

Stopping does not clear raised signals. `set()`, ordinary immediate `select()`,
and `try_select()` on a blocking `signal_tree` remain usable after stopping. A
ready signal may therefore still be selected after `stop()`. If `stop()`,
`set()`, and selection race, a timed selector may either return a signal or
return invalid while a signal remains raised; `stop()` does not impose ordering
on concurrent signal operations.

Stopping is permanent and may be requested more than once. There is no restart
operation. Before destroying a blocking signal set or tree, the owner must call
`stop()` and join or otherwise synchronize with every thread that could still be
executing a timed selection. Destruction while another thread is using the
object is unsupported.

## Typical worker loop

```cpp
void worker(bcpp::concurrency::signal_set & signals)
{
    auto hint = bcpp::concurrency::signal_id{0};

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
bcpp::concurrency::signal_set signals{executionContext.size()};

void make_ready(std::size_t i)
{
    signals.set(bcpp::concurrency::signal_id{i});
}

void service(bcpp::concurrency::signal_id id)
{
    auto index = static_cast<bcpp::concurrency::signal_id::value_type>(id);
    executionContext[index].run();
}
```

## Direct `signal_tree`

Most code should use `signal_set`. Direct `signal_tree` use is for lower-level or heavily tuned cases.

```cpp
bcpp::concurrency::signal_tree tree;        // deduces signal_tree<1>
auto hint = bcpp::concurrency::signal_id{0};

tree.set(bcpp::concurrency::signal_id{7});

auto selected = tree.select(hint);
```

Unlike `signal_set`, direct `signal_tree` is a lower-level object with a fixed natural capacity. Bad ids are a precondition violation. Use `signal_set` unless you have a specific reason to manage tree capacity yourself.

## Tree size

The default spelling:

```cpp
bcpp::concurrency::signal_tree tree;
bcpp::concurrency::signal_set signals{capacity};
```

uses the balanced default tree size, equivalent to `<1>`.

Explicit tree sizes are available:

```cpp
bcpp::concurrency::signal_set<0> tiny_or_heavily_sharded{capacity};
bcpp::concurrency::signal_set<1> balanced_default{capacity};
bcpp::concurrency::signal_set<4> larger_single_tree_domains{capacity};
```

`<0>` is a single 64-bit leaf tree. Larger values create larger physical trees and larger single-tree fairness domains.

## Selector policy

`select()` is policy-based. The default selector is the fairness selector.

```cpp
auto selected = signals.select(hint);
```

Other selectors can be supplied when appropriate:

```cpp
auto selected = signals.select<bcpp::concurrency::densest_child_selector>(hint);
```

Most users should start with the default selector. Alternative selectors are intended for specialized placement, benchmarking, or future policy-specific scheduling behavior.
