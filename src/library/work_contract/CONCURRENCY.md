# Work Contract — Concurrency Model

This document is the concurrency contract for `work_contract`. It states what is
thread-safe, the invariants the implementation rests on, the memory-ordering
assumptions (including the one dependency on `signal_tree`), and the known sharp
edges. The correctness argument is expressed in the C++20 memory model and
imports a release/acquire publication guarantee from `signal_tree`.

The load-bearing code is four methods in
[`work_contract_group.h`](work_contract_group.h):
`schedule`, `release`, `process_contract`, and `erase_contract`.

---

## 1. Substrate

A group and its handles share an intrusively reference-counted state containing a
fixed array of contract *slots* and two `bcpp::concurrency::signal_set` structures (from
`signal_tree`):

- `signalSet_` — pending **execution** signals (one bit per slot; "this contract
  wants to run / be released").
- `available_` — the **free-slot** pool (one bit per free slot).

Both provide a lock-free `select()` that returns a set signal to **exactly one**
caller and clears it atomically, and an idempotent `set()`. That exclusivity is
the primitive everything else is built on (independently stress-verified: 0
double-selects in 15M concurrent set/select operations).

The proof of those substrate properties belongs to `signal_tree`. WC treats them
as boundary guarantees and proves how its own state word composes with them.

---

## 2. The contract state word

Each slot has a single atomic word:

```
state_ :  [ generation : 48 | flags : 16 ]      std::atomic<std::uint64_t>
```

- **flags** (low 16 bits): `schedule = 0x1`, `execute = 0x2`, `release = 0x4`.
- **generation** (high 48 bits): the slot's incarnation counter.

Packing the generation into the same word as the flags is deliberate: a
`schedule`/`release` from a handle validates the generation as part of the very
CAS that mutates the flags, so there is no check-then-act window (see §5).

---

## 3. Lifecycle / flag state machine

The public lifecycle follows the state-machine diagrams in
[Work Contracts in Action (CppCon 2025)](https://github.com/CppCon/CppCon2025/blob/main/Presentations/Work_Contracts_in_Action.pdf):
idle contracts enter repeatable work, while release leads to one-shot
finalization. The tables below refine that presentation into the implementation
states needed for the concurrency proof.

Let `S` be the schedule flag, `E` the execute flag, and `R` the release flag.
Availability is separate from the state word: a free slot and an installed idle
slot both have zero flags, but only the free slot is present in `available_` and
has no callables in `work_`, `release_`, or `exception_`.

| Name | Flags | Meaning |
|---|---:|---|
| Idle | `000` | Installed, not scheduled, not executing. |
| Scheduled | `S = 001` | Work is pending; a signal is pending or already owned by a selecting executor. |
| Executing | `E = 010` | The callable is running. |
| Rescheduled while executing | `ES = 011` | The callable is running and must be signalled again on exit. |
| Finalization pending | `SR = 101` | Release is terminal and has pending executor work. |
| Finalizing | `ER = 110` | A selected release is running its handler and will be erased. |
| Release requested while executing | `ESR = 111` | The current callback finishes, then finalization is signalled. |

`R = 100` is not reachable through the public operations because release always
sets `S` with `R`. Other low flag bits are reserved and remain zero.

| Event | Before | After | Linearization and signal action |
|---|---:|---:|---|
| Create | Free slot | `000` | `available_.select()` owns the slot; install the work, release, and exception callables; optionally apply Schedule. |
| Schedule idle | `000` | `S` | Generation-validating CAS; set the contract signal. |
| Schedule already pending | `S` | `S` | CAS is idempotent; do not set another signal. |
| Schedule during execution | `E` or `ES` | `ES` | CAS records the request; the executing worker signals on exit. |
| Release idle | `000` | `SR` | Generation-validating CAS; set the contract signal. |
| Release scheduled | `S` | `SR` | CAS records terminal release; the existing signal is sufficient. |
| Release during execution | `E` or `ES` | `ESR` | CAS records terminal release; the executing worker signals on exit. |
| Select scheduled work | `S` | `E` | Atomic increment changes `001` to `010`; invoke the callable. |
| Finish normally | `E` | `000` | `fetch_and(~E)` returns the contract to idle. |
| Finish after reschedule | `ES` | `S` | `fetch_and(~E)`, then set the contract signal. |
| Finish after release | `ESR` | `SR` | `fetch_and(~E)`, then set the contract signal for finalization. |
| Select finalization | `SR` | `ER` | Atomic increment changes `101` to `110`; skip work and invoke the release handler. |
| Erase | `ER` or terminal derivative | next generation, `000`, free | Clear all three callables, store the incremented generation with all flags clear, then return the slot to `available_`. |

Schedule and release are idempotent in their already-requested states. Once `R`
is present, later schedules may set or preserve `S`, but cannot prevent the
terminal erase.

### Exception paths

Exception handling introduces no additional state-word value:

- If `work_` throws, that contract's exception handler runs while the slot remains in its
  executing state. The `auto_clear_execute_flag` scope guard then performs the
  same `E → 000`, `ES → S`, or `ESR → SR` transition as a normal return, even if
  exception handling rethrows.
- If that contract's release handler throws, its exception handler runs while finalization
  is active. The `auto_erase_contract` scope guard still erases the slot and
  advances its generation, even if exception handling rethrows.

A self-rescheduling contract loops through `E → ES → S`; a self-releasing
contract reaches `ESR → SR → ER` and is erased without another work invocation.

---

## 4. Invariants

These are the properties the implementation depends on. Breaking any of them
breaks correctness.

1. **Exclusive select ⇒ single-threaded contract execution.** Because
   `signal_set::select` hands a set signal to exactly one caller and `set` is
   idempotent (one bit per slot), at most one worker processes a given slot at a
   time. A callback never overlaps another invocation of itself, even though
   different contracts execute concurrently and successive invocations may use
   different worker threads. This is what lets a contract act as the single
   consumer or producer on an SPSC work-graph edge without internal locking.

2. **No schedule or release is lost at execution exit.** If a request CAS sets
   `S` before the executing worker clears `E`, the worker observes `S` in the
   value returned by `fetch_and` and signals the contract. If the worker clears
   `E` first, the request CAS retries against the idle state and signals the
   contract itself. These are the only two atomic modification orders, and both
   leave `S` paired with a signal.

3. **Release is terminal.** Once the `release` flag is set, the contract *will* be
   erased. Concurrent `schedule` bits are harmless: the next `process_contract`
   transition preserves `release` and routes to `process_release`, so the work
   function does not run again.

4. **One generation bump per recycle, at erase, coupled to return-to-pool.**
   `erase_contract` bumps the generation and calls `available_.set()` together, so
   "old incarnation ends" and "slot becomes claimable" are a single event.
   `create_contract` *adopts* the current generation (it does not bump again).
   The erase store is `release`-ordered and `create` acquires the slot through
   `available_.select`, giving a clean handoff of the new generation.

5. **Generation validated inside the CAS.** A `schedule`/`release` issued from a
   `work_contract` handle carries the handle's snapshot generation. The CAS's
   expected value contains that generation, so if the slot was recycled in flight
   the CAS simply fails and the operation is dropped — no ABA (§5).

6. **Exceptions do not bypass cleanup.** Execution and finalization install
   their cleanup guards before invoking user code. Every return or exception
   therefore performs exactly one normal execution-exit transition or one
   terminal erase.

---

## 5. ABA safety

A handle for slot *s* can outlive the contract it referred to — most importantly
when a contract releases *itself* from inside its own work while the caller still
holds the handle. After that release+erase, slot *s* may be reused by a brand-new
contract. The generation counter ensures the stale handle can never disturb the
new occupant:

- The handle remembers the generation it was issued at.
- `erase_contract` bumps the generation before the slot is reused.
- `schedule`/`release` compare the handle's generation against the slot's current
  generation **inside the flag CAS**. A mismatch drops the operation.

Because the generation check and the flag mutation are one atomic compare-and-swap
(not a check followed by a separate write), there is no window in which the slot
can be recycled between "generation looks valid" and "flag is set." That
check-then-act window is exactly the ABA hole; packing the generation into the
CAS closes it.

Validated by [`generation_test.cpp`](../../test/generation_test.cpp): a deterministic
self-release-then-stale-schedule test, a 500k-cycle generation-integrity test, and
a concurrent hammer that replays >1M stale-handle schedules against a churning
64-slot pool and asserts an exact run count.

---

## 6. Memory ordering

### WC's own atomics
- `state_` CAS: `acq_rel` on success, `acquire` on load / failed CAS.
- `erase_contract` store: `release` (publishes the bumped generation + cleared
  flags before the slot is returned to `available_`).
- `create_contract`: `acquire` load of `state_` to adopt the generation; the
  `available_` handshake carries the happens-before from erase to create.

### The one dependency on `signal_tree` (the boundary contract)
WC **publishes contract state through the signal handshake.** When thread A writes
`state_`/`work_` and then `signalSet_.set(s)`, and thread B does
`signalSet_.select()` → `s` → reads `state_`/`work_`, B must observe A's writes.
This requires:

> `signal_set::set` behaves as a **release** (or stronger) with respect to the
> caller's prior writes, and `signal_set::select` behaves as an **acquire** with
> respect to the caller's subsequent reads — forming a synchronizes-with edge
> through the tree's internal atomics.

`work_` in particular is a non-atomic `std::function`; it is safe only because of
this edge.

The current `signal_tree` implementation supplies this boundary: setting a leaf
uses a release operation and successfully selecting it uses an acquire-release
operation. Its own proof should establish that this remains true through every
tree level and blocking path; WC should consume it as a documented API guarantee
rather than re-prove the tree internals.

This synchronization is required by the C++ memory model on every architecture.
x86 TSO is useful implementation context but is not a substitute for a
`synchronizes-with` edge: relaxed signal operations would leave concurrent
non-atomic access to `work_` undefined even on x86. Correctness therefore must
not be architecture-guarded. Pinning the reviewed `signal_tree` revision keeps
this imported guarantee stable.

---

## 7. Thread-safety of the public API

| Operation | Concurrency |
|---|---|
| `work_contract_group::create_contract` | Safe from multiple threads concurrently (concurrent create never double-allocates a slot). See sharp edge #1. |
| `work_contract_group::execute_next_contract` | Safe from any number of worker threads concurrently. On a non-blocking group the no-timeout overload polls; on a blocking group it waits until work is executed or stop wakes it. Duration overloads bound the wait, and a zero duration is equivalent to `try_execute_next_contract()`. The named `try` overloads are available on blocking groups. |
| `work_contract_group::stop` | Terminal wakeup, not cancellation. After the application has stopped producers and drained all work, it may overlap only idle blocking execution calls waiting on an empty group. |
| `work_contract_group` destructor | Requires every thread that can access the group to have been joined. It invalidates outstanding handles; intrusive references keep shared state alive until the handles themselves are destroyed. |
| `work_contract::schedule` | Requires a valid handle at entry and is safe from any thread against a *live* group. If the generation becomes stale during the operation, the request is dropped; this is not permission to schedule a handle already known to be invalid. |
| `work_contract::release` / move assignment | Mutate the handle and must not race with another use of that same handle object. |
| `work_contract::is_valid` | May inspect an empty or live, non-mutated handle. |
| `this_contract::schedule` / `release` / `get_id` | Valid only from within an executing work function (owner path). |

---

## 8. Preconditions and library-contract undefined behavior

In this document, *undefined behavior* includes library-contract undefined
behavior: WC makes no promise about an operation performed without its stated
preconditions. A particular misuse may appear benign in the current build and
need not itself be a C++ language-level data race; that observation does not
make the operation supported.

The principal preconditions are:

- The result of `create_contract()` is checked before `schedule()`. Scheduling a
  default-constructed, moved-from, released, or failed-creation handle is outside
  the contract.
- Mutating a handle's ownership (`release`, move, or destruction) does not race
  with another use of that same handle object.
- `this_contract::*` is called only from the currently executing callback.
- The group remains alive until all threads that can access it have been joined.
- `stop()` is called only after producers have stopped, pending schedule/release
  notifications have drained, and callbacks have completed. It may overlap only
  idle blocking execution calls waiting on an empty group. No create, schedule, or
  execute operation starts after stop.

For a non-blocking group, ask workers to exit and join them before stop or group
destruction. For a blocking group, publish the application's worker-loop exit
condition, call `stop()` to wake idle calls waiting on the empty group, then join
them. Racing stop with a producer or with an executor that can still select work
is a lifecycle error, analogous to racing a standard container's `clear()` with
another operation. Synchronizing that lifecycle belongs to the application;
checking an internal one-way flag would add hot-path work without closing the
race for a thread that had already passed the check.

---

## 9. Sharp edges

**#1 — `is_valid()` cannot distinguish "pool full" from "already recycled."**
`create_contract` returns an invalid handle when the pool is full; but a contract
created *scheduled* can run, self-release, and recycle before you inspect its
handle, at which point `is_valid()` is *also* false. A naïve "retry
`create_contract` until `is_valid()`" loop therefore creates **duplicate**
contracts under concurrent execution. The safe idiom is to create
**unscheduled**, check the returned handle, then `schedule()` after you hold it.
The contract cannot run until you schedule it, so `is_valid()` reliably reports
whether creation succeeded. This is what `generation_test.cpp` does.

**#2 — handles may outlive their group, but cannot remain serviceable.**
Group teardown advances every slot generation before releasing its intrusive
shared-state reference. Outstanding handles retain the shared state safely, but
their generation checks fail, so `is_valid()` returns false and schedule/release
requests cannot modify the invalidated slots.

---

## 10. Validation status

- **Sanitizers:** the test binaries are clean under **ThreadSanitizer**,
  **AddressSanitizer**, and **UBSan** — 0 warnings/errors — including the 8-thread
  exact-count stress and the 500k-contract concurrent ABA hammer. TSan tracks
  happens-before per the C++ memory model (not x86 timing), so its clean result on
  the non-atomic `work_` field under heavy create/erase churn is positive evidence
  that the §6 publish edge is exercised correctly. Sanitizers are dynamic: they
  observe executions but do not replace the WC transition proof or the imported
  `signal_tree` proof.
- **Tests:** CTest covers the public API, lifecycle transitions, blocking and
  non-blocking execution, generation/ABA behavior, exception cleanup, and
  concurrent stress. Build with `-DWORK_CONTRACT_BUILD_TESTS=ON` (default) and
  run `ctest` from the build directory.
