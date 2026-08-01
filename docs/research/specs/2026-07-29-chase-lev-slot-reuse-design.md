# Chase-Lev Slot-Reuse Safety Design

## Problem

The experimental bounded Chase-Lev normal queue clears a stolen physical slot
after the thief wins the `top` compare-and-swap. Winning that compare-and-swap
immediately removes the logical entry from the deque and lets the owner reuse
the newly available capacity.

If the thief is descheduled after the compare-and-swap while the owner pushes
enough work to wrap the bounded ring, the owner can publish a new task in the
same physical slot. The delayed thief then stores `NULL` into that slot and
silently erases the new task.

This is isolated to the opt-in `LOCKFREE_NORMQ` path, but it is a scheduler
correctness failure and must be fixed before that path can be promoted.

## Safety Invariant

For every logical index in the live interval `[top, bottom)`, its physical slot
must retain the task most recently published for that logical index until the
owner or a thief claims that index.

After a thief advances `top`, the old pointer is outside the live interval. It
may remain stale because readers use `top` and `bottom` to establish membership,
and the owner overwrites the physical slot before publishing a later wrapped
logical index through `bottom`.

The original Chase-Lev algorithm likewise advances `top` without clearing the
stolen array entry:

- David Chase and Yossi Lev, [Dynamic Circular Work-Stealing
  Deque](https://www.cs.wm.edu/~dcschmidt/PDF/work-stealing-dequeue.pdf)

## Design

Remove the thief's post-claim `NULL` store. Keep owner-side clearing unchanged:
the deque has one owner, so an owner pop cannot race a later owner push from the
same end before the pop returns.

Add a test-only hook immediately after a thief successfully advances `top`.
The hook accepts a context pointer and is available only in
`LLAM_ENABLE_TEST_HOOKS` builds. Production builds have no hook state or call.

Use the hook to construct a deterministic regression:

1. Push one task at logical index zero.
2. Start a thief and stop it immediately after its successful `top` CAS.
3. Push exactly `LLAM_NORM_QUEUE_CAP` replacement tasks from the owner.
4. Verify the final push wraps to physical slot zero.
5. Release and join the thief.
6. Pop all replacement tasks in LIFO order and verify none was lost.

The test must fail against the old implementation because the delayed thief
clears the wrapped task in physical slot zero.

## Alternatives Rejected

- Per-slot sequence numbers would also distinguish logical generations, but
  add metadata and extra atomic traffic to every queue operation. The current
  access protocol does not need them when thieves stop mutating claimed slots.
- A probabilistic stress test would exercise the race but could not guarantee
  the critical post-CAS scheduling window on every platform.
- Reducing usable capacity to avoid immediate reuse only delays the same
  wraparound race and wastes bounded queue space.

## Verification

- The focused internal shutdown/invariant test must fail before the production
  change and pass after it.
- The same regression must pass repeatedly to prove the gate is deterministic.
- The full host suite and sanitizer-supported adjacent checks must remain
  green.
- A focused security diff review must confirm that the test hook is absent from
  production objects and that no new scheduler ownership path was introduced.

This change does not promote `LOCKFREE_NORMQ`, alter the public ABI, change the
LEIR performance decision, or authorize a version bump or release.
