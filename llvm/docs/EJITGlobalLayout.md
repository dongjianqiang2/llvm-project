# EJIT Global Function Layout

Issue #245, spec v0.4 (2026-10-13), baseline
`e09df842062735ba9f6279ae7d1c5b43fd723bec`, extended 2026-10-15 with
unlisted-function support and the Failed-to-ordinary-fallback semantics.
The 2026-10-19 integration fixes terminal-state PGO, real-return draining,
complete bound-cache identity and retained profile ownership.
The former v0.3 pending-scope/dimension-major checkpoint is superseded.
Production implementation and native tests are present. SDK final linking,
cold core6/core16 board acceptance and performance acceptance remain pending.

## Frozen Contracts

The coordinator-approved invariant covers EVERY declared final Baseline body
(PGO off), or final PGOUse/T2 body (PGO on), of one selected complete generation.
Temporary T1 and prior/retired generations are not moved or included in that
generation's order; their real execution lifetime and memory costs remain protected.
No late, cold, inactive or insufficiently sampled final member is optional.

The fully expanded manifest array position is authoritative global rank, across
ALL preparation batches and sampling waves. Dimension identities do not re-sort
the caller's list. V1 takes an explicit list, not an implicit expansion template.
If a generator offers a function template, it must display/freeze its complete
cell/TRP/version expansion before calling initialization.

V1 provides sequential safe publication after ALL bodies have linked, passed
actual address/range validation and sealed. It does not promise all-entry atomic
visibility. Mixed still-valid old/new dispatch is permitted during cutover.
Partial failure retains published resources, reports published/unpublished ranks
and remains FAILED, never COMPLETE. A new explicit refresh command or actual
lifecycle event can start a complete in-process generation; invalid identities
are never reused or retried as an unordered subset. Residual Tier-2 re-arms
that were armed from a saturated Tier-1 before publication are dropped once
the layout has delivered that identity's Tier-2 (a published slot or a parked
collective-append body): they never replace a delivered rank-ordered body
after COMPLETE, neither at dequeue time nor at the pending flush.

**Failed semantics (changed 2026-10-15):** a FAILED layout no longer switches
misses to OffMode. Every function — manifest member or not — falls back to the
ordinary shared-pool paths (batched deferral, immediate Tier-1, queue-drain
Tier-2) and keeps compiling, publishing and dispatching normally. Parked
unlisted requests and deferred batch markers are released on failure so fresh
misses re-enqueue instead of wedging on AlreadyPending. The system keeps
running; the status API is the only failure visibility and products must poll
it as a health check to notice a lost layout.

## Unlisted Functions (added 2026-10-15)

The manifest no longer has to cover every registered entry. Functions that are
registered but absent from the manifest ("unlisted") compile through the
ordinary paths and append after the manifest bodies:

- **Collecting phase.** An unlisted Baseline miss parks in the deferred batch
  backlog (staged request marker, no near-pool allocation). An unlisted
  Tier-1 compiles immediately into the far pool, unordered, as always. An
  armed unlisted Tier-2 first waits for all real T1 returns, then parks
  uncompiled in a bounded owner-private list (queue capacity entries) and
  releases its sampling admission, so
  parked entries never starve the concurrent-profile slots; overflow discards
  the request and the function stays on its live Tier-1 until a later re-arm.
  The `unlistedReady` status field reports the parked count (Tier-2 parks plus
  batch backlog) during collection.
- **Freeze point.** The collective set freezes at the all-ready transition.
  Requests still resident in the shared queue or waiting for a real T1 return
  at that instant take the ordinary post-freeze paths; only what is already
  drained and parked or deferred joins the collective window. A held call is
  never counted as a completed sample merely because its hit count saturated.
- **Collective window.** After the manifest bodies link in rank order, the
  parked unlisted Tier-2 entries compile (funcIndex ascending, re-arm
  duplicates deduplicated to the latest), then the deferred batch requests
  compile in the existing dims→funcIndex→tier order. One seal covers every
  allocation made in the window. Manifest members publish in rank order;
  the appended unlisted bodies publish through the ordinary auto/explicit
  flush after COMPLETE. A saturated unlisted Tier-1 re-arms on every lookup;
  each re-arm supersedes the identity's earlier park (latest wins), so one
  hot function cannot fill the bounded park list. The `unlistedAppended`
  status field reports how many unlisted bodies actually joined the window;
  the COMPLETE diagnostic summary reports the member counts, the appended
  set and the final address-order verdict (per-rank bodies appear in the
  LINKED diagnostics).
- **Precision kill retained for exact declarations.** A funcIndex the
  manifest declares with one exact identity still FAILS the layout when a
  different identity of that funcIndex arrives: naming a function with an
  exact identity makes every other identity for it a deployment error, not an
  append candidate. A name-only declaration (empty identity) exempts its
  function instead: the first admitted identity is captured as the member
  body and every later identity of that function routes as unlisted —
  ordinary compile, post-manifest collective append, layout unaffected.
  Entirely undeclared funcIndexes never fail the layout.

## Manifest and Identity

V1 requires the batched NO_RECLAIM shared SRE code pool.
`Config::globalLayout` / `ejit_init_global_layout` freezes an explicit
initialization list; since 2026-10-15 the list does not have to cover every
registered entry (see "Unlisted Functions"). The additive C API leaves
`ejit_config_t` unchanged.
Names resolve read-only through `EJitFuncRegistry`; a manifest entry that names
an unregistered function is still rejected. A declaration carries either the
exact registered dimension shape, instance versions and bound
argument/address/extent descriptors, or nothing but the name: a **name-only
entry** (no dims, no bounds) captures the identity of the first admitted
compile request of that function at collection time and makes it that rank's
body. Name-only entries exist for one-version-per-function deployments whose
instance choice is a runtime fact (which cell/TRP combination actually
dispatches); the version policy rule is unchanged (policy0 captures live
versions at first active admission). An exact entry keeps the precision
contract — another identity of that function is a deployment error — while a
name-only entry exempts its function: later identities of it route as
unlisted (see below) and never fail the layout.
`sharedOwnerElected` validates loader metadata and bound argument indices.
`configureGlobalLayout` rejects duplicates, unsupported
shapes, invalid descriptors, dispatch-key collisions and bucket capacity overflow
as one initialization failure, with no partial installation.

Completeness of instance expansion is caller-declared: V1 cannot infer which
inactive cell/TRP/version combinations a product intends to require. Every
supplied member remains mandatory; unlisted registered functions are
deliberately outside the collective contract.
Explicit manifest members with colliding legacy dispatch keys still reject
initialization. For name-only/unlisted requests, distinct bound identities can
coexist in the bounded full cache: lookup, pending markers, final publication,
drain checks and obsolete-T2 detection all compare argument/address/extent.
They neither overwrite nor suppress another bound identity. Bound roots bypass
descriptor-blind fast caches from their first bound admission.
Registered dimension formal order is kept for legacy dispatch compatibility;
it is not layout rank. Manifest/source-bitcode/configuration/budget/bound identity
participates in the shared peer fingerprint. Request equality is structural,
including owner generation, dimension/version pairs and complete bound descriptors.
Stable T2 lookup does not query or sort the manifest.

## Preparation Boundary

`EJitOrcEngine::prepareBitcodeModule` parses and isolates the entry, resolves
registered imports, consumes borrowed bound views, applies the SAME extracted
`optimizeModule` routine as the immediate path, then calls the configured
`IRCompileLayer::getCompiler`. The artifact owns a relocatable `MemoryBuffer`,
entry/tier/cache identity and import symbols, not already-relocated machine code.
No JITDylib object submission, executable allocation or callable lookup occurs
until `linkPreparedObject`. `EJitCompileDriver::prepareNow` is wired to the real
shared worker; it is not a test-only planner.

Reachable mutable/address-significant data requires canonical registered backing
through existing period/static/user symbol registries. It becomes an external
import of the original storage, never a copied mutable initializer. Only proven
address-insignificant immutable data can remain private. Unused local globals
are pruned after other roots are isolated, preventing unrelated data from being
replicated into every object. Unsupported backing fails conservatively.

## Normal PGO and True Returns

T1 still compiles/links immediately in the far pool and executes real samples
under the unchanged admission limit. A sampling lease is acquired BEFORE
incrementing the dispatch hit count; the normal wrapper leave releases it only
after the real call returns. Saturation closes new T1 dispatch; waiting calls use
legal AOT/old fallback. `samplingDrained` requires threshold hits AND true returns
AND zero executing leases. Worker polling remains nonblocking while draining.

The complete counters/helper inventory, verified ICP/memop and scalar/audit
metadata are frozen through existing snapshot protocols. Sibling VP snapshots
remain owned and charged until their own identity is prepared. Helper profile
names include full specialization identity to prevent cross-root conflation.
Root Gen/Use/VP names use that same identity during profiling and restore the
exported entry name before code emission. Counter, audit and VP inventories
also use the full identity, not the legacy function/dimension-only code key;
several drained bound versions can wait without replacing each other's data.
Missing/inconsistent profiles and failed snapshots cannot fall back to an
edge-only pretend success in global mode.
This applies to ordinary unlisted and FAILED-layout T2 as well as manifest
preparation. Complete synthesized profiles remain owned until their exact T2
publishes or the generation retires, so discarded artifacts can fall back
without losing counters or sibling VP. VP collection closes exactly once per
identity. A failed, consuming capture cannot be retried as a partial profile;
a fresh generation is required to restart collection, with legal fallback in
the meantime. Cached complete profiles remain usable.

Global-layout VP uses bounded site probing (same existing capacity), not
direct-map sibling displacement. Full tables, out-of-range cores or preempting
same-core writers latch a failed round instead of silently losing data or
spinning under preemption. Existing nonglobal bounded approximation remains
unchanged. The lossless label concerns site ownership, not unbounded retention
of arbitrary value distributions; existing K-way heavy hitters still apply.

Only AFTER an owned PGOUse artifact exists does `releaseSamplingAdmission` free
the drained member's slot. This is distinct from `finishPgoFunction` publication;
it neither claims T2 publication nor fabricates samples. Ready markers prevent
resampling while final layout waits. T1 counters/side metadata can then be released;
the emitted object no longer borrows them. Admission remains funcIndex-based as
in the existing pool, so instances of one entry advance sequentially.
Publication of an already-released identity must not release the admission of
a newer identity of the same function. Sampling is open in COLLECTING,
COMPLETE and FAILED, and closed during collective linking/publication and
refresh. Ordinary late arrivals can therefore finish their full sampling
quota. A member waiting for its final real leave when layout fails is requeued
after return without requiring another business lookup.

## Ordered Linking and Cutover

`serviceGlobalLayout` gates linking on required == ready, rechecks identities,
and submits every object in explicit rank order. Fresh serial-tagged JITDylibs
retain older generations. Existing JITLink, lazy imports, graph helpers/GOT,
alignment and code-pool routing determine actual size, not fixed per-function slots.
Actual entry addresses must strictly increase; each finalized owning executable
range must contain its body and be Near. Dynamic allocator address reversal or
unexpected routing fails, never produces a sorted-pointer success claim.

The existing batch seal/flush and code-readiness/range callbacks run before the
first cache publication. Shared lookup retains existing per-core preparation
guards. An intermediate `flushCodeBatch` request being serviced is NOT global
COMPLETE and cannot link/seal a collected subset. State transitions use guarded
CAS so concurrent lifecycle invalidation cannot be overwritten by COMPLETE.

Already-shared/aliased identities are not exempt from order checks. V1 rejects
legacy-key collisions, emits independent closed graphs only with canonical data
backing and requires strictly increasing body addresses. It does not claim
integration with unmerged #230/#231 common-profile/shared-body mechanisms.

## Budgets and Shutdown

Defaults: 1024 members (also queue/bucket capacity constrained), 64 MiB objects
and import metadata, 32 MiB frozen profile/side metadata, 16 MiB each Near/Far
retained reservation, and 1,048,576 collection worker polls including drain wait.
Retained synthesized profiles and pending sibling VP storage share the profile
budget. The pending VP record cap uses a quarter of the remaining budget.
Poll budgets are not wall-clock or hard-real-time compiler deadlines.

Persistent shared Near/Far reservation ledgers charge complete pools, raw alignment
slack, failed holes, helpers/GOT/data and all retired NO_RECLAIM generations.
Untracked legacy owner history cannot switch to layout ownership, and retained
layout allocations cannot switch to legacy ownership without a cold reset; this
prevents reuse of old pools whose retirement offsets cannot be proven.
Fixed-region offsets survive compatible compiler-owner changes; old legal addresses are not
overwritten. Exhaustion is explicit fallback, not reserve enlargement. Object
buffers are released on failure or after finalized validation; linked/partially
published code remains charged and retained. A failed generation closes/drains
remaining T1 rounds before releasing their sampling ownership.

Shutdown stops/joins the worker and uses a bounded wait for real T1 leaves before
slot reuse or compiler destruction. The C API defers shutdown while live leases
remain, retaining its runtime for a later retry. Direct C++ destruction with an
unreturned T1 retains one compiler and leaves the shared domain Stopping: this
conservative terminal retention is preferable to UAF, not a successful shutdown.
Platform obligations for stable NO_RECLAIM code/registered data remain unchanged.
Shared ABI is 21 and VP ABI is 3: rebuild every participating image together.

## Verification Boundaries

Actual-source state tests cover missing members, explicit cell/TRP rank, invalid
manifests/bindings, object budget, preparation/link/order/seal failures, stale
versions, partial cutover and retained reservations. They do not alone prove
real machine-code order. Actual compiler integrations cover A/H/Z, reverse and
permuted arrivals, intermediate flushes, helpers/registered mutable backing,
cross-4KiB and cross-2MiB graphs, live T1 shutdown, and real four-slot/20-target
PGO with exactly 160 preserved indirect-call observations before final linking.
Real cell/TRP/bound Baseline and PGO cases exercise the expanded identity path.

QA01-QA11 have native evidence, but this is not comprehensive product acceptance.
SDK/BE final DLIB linking and QA12 cold core6/core16 board execution are pending;
`ejit_test/ejit_global_layout_sre_test.c` is a source fixture, not a board result.
QA13 overhead/latency/I-cache/iTLB/business-cycle measurements remain pending.
No layout speedup, board readiness or issue closure is claimed. Raw commands,
flags, counts and retained failure logs are recorded in the task support RESULT.

## In-Process Generation Refresh (Review Follow-Up)

Normal lifecycle turnover is not terminal until process restart. The owner worker
enters REFRESHING (status9), closes T1 dispatch, nonblockingly waits for true
returns, drops only unexecuted objects/obsolete queued work, and resets complete
old profiling ownership AFTER drain. Both VP payload halves are cleared under the
same drained-writer contract; per-wave snapshots remain preserved within a
generation. The compiler, old JITDylibs, data/code allocations and reservation
ledgers remain alive. Advancing the shared generation invalidates old dispatch
slots/late requests; legal AOT remains available while the entire new manifest is
prepared and linked again. The owner/worker is not restarted or reelected.

The default declaration version policy0 ignores supplied version numbers and
captures live versions at first active admission, so cold init followed by
activate works without hardcoded1. A member with inactive dimensions remains
required, not silently removed. Policy1 explicitly asserts all dimensions are
active and match the supplied versions NOW; inactive, malformed or stale inputs
reject the whole initialization/replacement immediately with function/dimension/
instance/expected/live diagnostics. A later real turnover creates new live
identities, rather than treating an old assertion as permission to execute stale
code. Caller rank and full binding identity are retained across automatic rebuilds.

`ejit_refresh_global_layout` / `EJitCompileDriver::refreshGlobalLayout` copies a
complete owner-side replacement manifest and returns PENDING/accepted. It can
change explicit rank, instance expansion and bindings within the frozen registered
catalog; like the initial manifest it may list any subset of the registered
entries — parked unlisted requests and deferred batch markers of the retired
generation are released (markers dropped before the generation bump) and the
new generation collects from scratch. Invalid input leaves the current legal layout installed. The worker
publishes a matching new registration fingerprint; attaching during REFRESHING
is deferred. Opt-level/triple/mode/budgets and the registered source catalog are
initialization settings, not silently changed by a manifest refresh. Their
existing quiescent reinitialization path remains available; global-to-global
owner generations retain the same retirement ledgers, not a process-only reset.

Published slots carry complete opaque bound identity, compared under existing
lookup validation without a manifest scan or private data copy. Bound roots do
not use descriptor-blind L0/inline shortcuts. This prevents a replaced binding
from serving a body compiled with another binding's facts. Keep bound backing
alive through its real compile/execution consumers, including prior generations.

A transient sampled-slot lease returns Retry, not Failed. The worker retains
linked/sealed addresses and the published subset, checks identity again, and
retries boundedly without recompiling, reallocating or resealing. T1 publication
uses the same retention rule during a refresh. Real T1 execution is closed once
collection freezes and reopens for ordinary rounds after COMPLETE/FAILED;
a preempted acquisition that later rejects cannot execute T1.
Retry and refresh-drain budgets use maxCollectionPolls; exhaustion stays terminal
until a NEW lifecycle event or explicit command, never a self-renewing budget.

Audit-only requests are honored, not silently disabled. They use real temporary
sampling and the same drained admission/object path; the final optimizer runs
ordinary Baseline, not PGOUse guidance. Its served slot uses the existing terminal
T2 marker to prevent resampling. Ordinary PGO branch audit and VP remain compiled
ON and tested separately. `ejit_shutdown_checked` reports PENDING for deferred
shutdown; the existing void API remains compatible. A retained C++ compiler keeps
the domain Stopping, so new owners cannot accumulate while that lease is live.

Native regressions include three complete real cell/TRP+bound generations,
automatic turnover, explicit reverse-rank/binding replacement, first activation
with zero-initialized versions, explicit-stale rejection, real T1 refresh drain,
bounded terminal recovery, and paused lookup/publish retry in NO_RECLAIM builds.
These do not establish SDK/board/performance acceptance. The hot result remains
16 bytes on tested LE/BE compilation; packed flag masks have no measured target
cycle guarantee. Shared queue requests are now208 bytes on64-bit (168 on32-bit),
so the current ABI21 requires all participating images and queue users rebuilt
together.

Native ABI probes of the actual published/current headers (32 buckets,16 slots,
1024 queue entries) report a16-byte lookup result in both builds. Queue requests
grow200->208 bytes, cache slots224->360, and the shared state358080->440000 bytes
(+81920 bytes/80 KiB). The unlisted-functions status fields
(`globalLayoutUnlistedReady`/`globalLayoutUnlistedAppended`, mirrored into
`ejit_global_layout_status_t` as `unlistedReady`/`unlistedAppended`) fit in
the old blob's total allocation: the shared state stays 440000 bytes, but fields
after the inserted counters move. Equal total size does NOT make the layouts
compatible; ABI21 rejects ABI20 peers. `ejit_global_layout_status_t` is now 64
bytes, so its callers must also rebuild with the matching header. This is real
compatibility/space cost, not a speedup claim.
Audit-only fixture coverage has zero new ICP observations; ordinary PGO preserves
160 and the unmodified64-hit cold configuration preserves1280 across20 roots.
Owner control commands and shutdown must be serialized with the existing runtime
object lifetime; the compiler worker processes accepted refreshes concurrently.
Owner shutdown still uses its existing bounded synchronous yield loop; checked
shutdown adds observability, not a hard-real-time return guarantee. LLVM compiler
scratch and externally owned input/backing memory are not hard-capped by staged
object-byte limits. Retained pools, staged owned objects/profiles, and member
counts remain bounded as described; target peak memory is still a QA13 gate.

NO_RECLAIM is still intentional: repeated successful refreshes consume retained
Near/Far capacity. They are not an unlimited-cycle reclamation promise. Once
those bounded reserves exhaust, the generation fails with legal fallback;
safe region reclamation requires a separate lifetime/platform contract.

## Additional Board Fixtures

`ejit_test/baremetal/ejit_global_layout_unlisted_test.c` covers a partial
manifest with ordinary PGO. `ejit_global_layout_nameonly_test.c` covers first
identity capture, a second identity and lifecycle refresh. They are separate
images, not sources to link together. The name-only fixture requires all six
identities to reach T2, the second identity above all manifest bodies, and zero
remaining sampling admissions before PASS; perpetual AOT service is not success.
Both fixtures default to no round deadline (`*_WAIT_ROUNDS=0`). Positive
overrides bound automated runs; real API errors and incorrect results still
fail immediately. Native/BE compilation is not a new board acceptance result.
