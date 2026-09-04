# EJIT Cold Profile Reclamation

Online PGO admits a bounded number of functions into pre-link profiling. A
function that stops making real Tier-1 dispatch progress must not occupy one of
those admissions forever.

## Timeout

`EJIT_SRE_PGO_COLD_PROFILE_TIMEOUT_TICKS` is a build-time unsigned 64-bit
timeout. Its default is `300000000000`; `0` disables reclamation and emits one
diagnostic. On SRE the unit is the platform cycle-counter tick, so board builds
must convert the desired wall time using the deployed counter frequency. It is
not a worker scheduler delay.

Only the owner worker initializes and updates profile timestamps. It observes
every change in the exact Tier-1 slot hit count on both busy and idle worker
steps. Producer cores therefore do not need synchronized cycle counters.

## State And Failure Rules

Each admission carries the complete specialization identity, lifecycle
versions, owner generation, and a unique request token. Cold reclamation and
all compile failure paths release only an exact token. `Tier2Compiling` is not
interrupted, but every return from compilation must either enter linked-pending
or terminate the admission.

An expired identity is recorded in a separate exact suppression table, not in
the executable result cache. Its capacity is controlled by
`EJIT_SRE_PGO_COLD_SUPPRESSION_CAPACITY` and defaults to 128. Empty slots are
used first; a full table uses deterministic round-robin replacement. Replacing
an old record only allows that identity to attempt profiling again and never
changes AOT correctness. With the default build the table occupies 9,216 bytes;
the complete shared state is 432,576 bytes and remains below its 512 KiB test
budget. The build rejects capacities above 128, and a production static assert
also rejects a taskpool blob above 512 KiB. A 4,096-entry table would make the
state 718,272 bytes. The blob is not the only object in `.mc_shared`; every
board image must still verify the complete section against its linker-region
capacity using the final link map.

The same lifecycle version stays on AOT. A generation change, or a version
change observed for the exact suppressed identity, clears its function gate
and permits a new profile. Because value-profile storage is per function, a
different cell of the same suppressed function does not independently recover;
it remains on AOT until that reliable lifecycle boundary or deterministic table
replacement. This is intentionally conservative. In `NO_RECLAIM` builds the
old physical Tier-1 allocation is never freed.

## Automatic Publication

Linked Tier-2 code remains owner-private RW/NX. Automatic pool publication
requires all of the following to remain stable across the debounce window:

- no pre-link PGO admissions;
- an empty compile queue;
- no compile in flight;
- no producer in the admission-to-enqueue window;
- an unchanged activity epoch.

The debounce is `EJIT_SRE_PGO_PUBLISH_QUIET_CYCLES`, measured by the owner
worker's monotonic cycle counter. It defaults to `3000000000`, one hundredth of
the default cold-profile timeout and 90% below the former 30000000000-cycle
default. It should be tuned from the deployed counter frequency and the longest
expected gap between profiling waves. PGO activity restarts the complete
window. Capacity and explicit publication remain immediate overrides.

A publish barrier makes concurrent misses fall back to AOT while the final
snapshot is checked and pools are sealed. Pools commit independently. A failed
pool cannot block successful pools. Each linked Tier-2 publication receives at
most three attempts with a 32-worker-step backoff; permanent failure performs
token-exact cleanup and leaves the identity safely on AOT.
