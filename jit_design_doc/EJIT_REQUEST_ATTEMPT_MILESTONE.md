# Request-attempt lifecycle milestone

Status: experimental first runtime-integration milestone for PR230. The code is
default-off and is not yet enabled by `EJitCompileDriver` or a public C API.
Tests opt in through `EJitSharedTaskPool::setRequestAttemptsEnabled` before
owner initialization. This milestone does not implement representative group
selection, collector sessions, `ProfileBundle`, prepared-code reuse, or shared
logical aliases.

## Identity and capacity

Every opted-in logical request receives a nonzero 64-bit token before it can
claim deduplication or PGO admission. The shared counter is monotonic across
owner shutdown/re-election and stops at `UINT64_MAX`; it never wraps. A request
without a token fails closed to its AOT fallback.

The dedup slot stores the exact token. A cancelled R1 may therefore release its
claim and allow R2 for the same function in the same owner generation, while a
delayed R1 queue item, compiler return, or publication callback can only clear
R1's token. It cannot clear R2.

ABI v20 also records the originating token in every published cache slot.
Tier-2 reacquires that exact slot and validates token, generation, dimensions,
versions, admission ownership and the stored bound descriptors under one cold
claim operation. A stale cell cannot borrow another cell's active attempt.

Live records and completion history have independent fixed capacities. A
finished attempt immediately leaves the live table and writes a bounded
diagnostic tombstone. Retained history never consumes live admission. The live
table defaults to 256 entries and history to 64; exhaustion falls back to AOT
without an independent compile or retry loop.

## Three separately owned events

`SamplingFinished` is owned by the PGO admission slot. The representative T1
request keeps it pending while its profile is active. Tier-2 success, terminal
failure, lifecycle cancellation, generation loss, or shutdown releases the
matching `(funcIndex, attemptToken)` admission exactly once. A callback for an
old token cannot release a newer admission.

`CompileBorrowEnded` is owned by the worker/compiler boundary. T1 callback
return is not the end because Tier-2 may read the borrowed objects again. The
event settles after the last successful Tier-2 compiler read, or after a
cancelled/stale queue item is acknowledged. Owner shutdown may settle it only
after the worker has stopped and joined. A timeout alone never settles it.
Descriptor bytes are cleared when the live record retires and are never copied
into delayed publication records.

`LogicalPublished` is owned by executable cache publication. T1 publication
does not settle it. It settles only after the final Tier-2 pointer reaches the
Ready cache state, or is closed by cancellation/failure. Linked RW/NX state is
still pending. Every delayed publish rechecks that its exact attempt remains
live and uncancelled before it may update the cache.

Cancellation and cache publication use the same bucket-then-attempt lock order
for their final decision, without holding either lock across compiler, LLVM or
platform callbacks. Cancellation that wins prevents the Ready transition;
cancellation after a visible commit retracts only the exact token-bearing slot.
Retracted code remains physically owned by the code pool so NO_RECLAIM readers
and an in-progress compiler cannot observe freed storage. Removing a cancelled
T1 slot makes the same identity a real miss again and permits a fresh token.

An attempt retires only after all three pending bits are clear. Event delivery
is idempotent and independent of order, including borrow-end before cancel.

## Real paths covered

The token is carried by `EJitCompileRequest` through the shared MPSC queue,
generation/version checkpoints, T1 publication, later T2 enqueue, compile
callback return, queue-full rollback, pending batch publication, lifecycle
toggle, cancellation, and owner shutdown. Existing behavior remains selected
when request attempts are disabled.

The opt-in policy accepts only Async plus normal online PGO. Its shared mode and
PGO controls are immutable after Ready. Unsupported combinations fail before a
request record, dedup claim, PGO admission, or queue item is created.

## Remaining integration

The product configuration and wrapper/runtime C ABI do not yet enable or expose
the protocol. A later milestone must return tokens and completion status to the
caller that owns borrowed-object lifetime, then connect representative groups,
isolated sampling sessions, the complete immutable profile bundle, and prepared
physical-code publication. Until that adapter exists, this is reviewable
taskpool infrastructure rather than a deployable sharing feature.
