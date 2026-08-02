# Future Work

## Optional forward-only traversal

Signal selection currently favors availability over strict forward traversal. After
a selector reserves a path through the parent counters, it may select a signal
preceding the hint within that subtree when doing so is necessary to complete the
reservation and preserve the parent/child sum invariant.

A future compile-time traversal policy could make strict forward traversal
selectable. Until multiple traversal policies exist, the current behavior does
not need a policy name.

Forward-only traversal would require restoring each abandoned child count while
unwinding, locating the next eligible sibling, reserving the new path, and
descending again. This uses the same reservation accounting and concurrency model
already needed when setters and selectors cross paths; it does not introduce a
new class of synchronization. It does, however, require additional atomic
operations, CAS retries under contention, and cache-line traversal.

Strict forward traversal would guarantee that selection does not intentionally
move backward before exhausting the forward work it observes. It could not
provide snapshot-perfect ordering while signals are concurrently changing.

This is intentionally shelved. Reconsider it when a concrete consumer requires
depth-independent forward ordering, and evaluate it on an isolated branch with
correctness tests, service-distance measurements, and throughput benchmarks
before deciding whether to retain it.

## Indexed forest of complete trees

For very large capacities, consider using a forest of relatively small complete
signal trees, connected by one or more directory trees. Each real tree root
continues to count its ready signals. A directory entry instead represents whether
a real tree, or a group of real trees, is nonempty. The directory hierarchy would
therefore change only when a real tree crosses between empty and nonempty, rather
than for every signal operation.

A hint could identify both the preferred real tree and the preferred signal within
that tree. Selection would use the directory to locate the next nonempty tree and
then traverse a shallow real tree. This may retain near-small-tree traversal cost
at large logical capacities while preserving priority-oriented hint behavior.

The critical correctness problem is the transition race between a selector taking
the last signal from a real tree and a setter repopulating that tree. Clearing the
directory entry must include a recheck or handshake that guarantees a nonempty
real tree can never become invisible. The directory also adds another cache line
and atomic transition when a real tree crosses empty/nonempty.

This is an architectural experiment, not current work. Prototype it on an isolated
branch and compare it with both deep trees and ordinary signal sets at equal
logical capacities.

## 32-bit nodes

A 32-bit-node benchmark would be inexpensive future evidence, but there is no
current reason to expect a throughput win on 64-bit x86. It would reduce fanout
and deepen or increase the number of trees, while locked 32-bit and 64-bit atomic
operations still pay essentially the same cache-coherence cost. Keep the current
64-bit design unless measurement shows otherwise.
