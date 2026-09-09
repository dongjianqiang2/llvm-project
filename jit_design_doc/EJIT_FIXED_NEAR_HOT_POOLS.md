# Fixed Near-Hot Code Pools

This opt-in layout is the alternative to the old cross-function batch sorter.
It does not stage LLVM object files and it does not sort after JITLink. Every
non-Tier-1 allocation is linked immediately into its semantic pool; only the
RW/NX to RX transition and cache publication wait for a quiet queue.

## Layout

The default near-hot pool capacity is 36 MiB:

* `cell[0]` through `cell[15]`: one 2 MiB, 2 MiB-aligned pool each;
* no `periodName == "cell"`: one 4 MiB public pool;
* Tier-1 instrumentation: the existing dynamic far pool.

The example linker script reserves 38 MiB: 36 MiB of usable pools plus up to
2 MiB of start-alignment slack.

`EJIT_CODE_POOL_NEAR_HOT_CELL_BYTES` and
`EJIT_CODE_POOL_NEAR_HOT_PUBLIC_BYTES` are CMake cache variables and matching
compile-time macros. Both must be positive multiples of the 2 MiB split
granule. A custom value also requires a linker script `.text.ejit` reservation
of at least `16 * cellBytes + publicBytes`, plus any alignment slack.

Pool selection is made before `addIRModule`: the compile context carries the
verified lifecycle metadata and the selected pool ID is associated with the
exact internally-created JITDylib. The JITLink memory manager looks up that
exact JITDylib metadata; it never parses a function name or guesses from an
address. A missing, duplicate, malformed, or out-of-range `cell` declaration
returns the compile to AOT. The complete absence of `cell` is valid and maps
to public.

## Publication

ORC/JITLink allocates and fixes up near code while its pool remains RW/NX.
Pending ranges are owner-private and are not returned by the finalized range
query, so no cache, inline-cache cell, or shared `fnPtr` can observe them.
ABI v22 adds a 256-entry shared linked-pending identity registry. A Tier-2
request enters it before releasing its PGO admission, function gate, and coarse
dedup claim. Producers consult it only after a shared-cache miss, so eviction
of the Tier-1 slot cannot restart sampling while the linked Tier-2 waits for
publication. The exact registry check and per-function dedup claim share one
critical section. If publication changes the dispatch epoch after a cache
miss, the producer drops the registry lock and retries the cache lookup; after
a bounded number of retries it stays on AOT. If the registry is full, the
request retains its original claims until publication and remains safely on
AOT.
After two empty worker observations, or an explicit
`ejit_publish_pending_code`, the owner commits each pool independently:

1. seal that pool's pending 4 KiB pages with `enable_ex`;
2. verify the ranges are finalized and publish only that pool's cache entries;
3. leave failed pool ranges available for an explicit pool retry (or terminate
   the request with AOT fallback according to the failure contract) while
   other pools remain published.

An enable or cache-publication failure does not cause an idle-worker retry
loop. Diagnostics include a 17-bit failed-pool bitmap. The linked bytes are bounded by
`EJIT_CODE_POOL_FIXED_NEAR_HOT_PENDING_LIMIT` (256 by default, above the
expected approximately 150-version batch). At capacity, the owner attempts a
controlled flush; if that fails, new results fall back to AOT. Under the
current NO_RECLAIM contract, linked bytes from a failed allocation may be
reported as stranded and are not claimed to have been physically reclaimed.

The legacy `pendingBatchCompiles_` dimension comparator and enqueue watermark
are disabled for this layout. Baseline and PGO Tier-2 use the same near pool
and publication rules; Tier-1 keeps its immediate far-pool route and existing
throttle/delay behavior. Executable split sections such as `.text.ejit_cold`
remain in the selected JITDylib and its semantic cell/public pool; this change
does not introduce a separate MFS or cold pool. Diagnostics report the flush
reason (`idle`,
`explicit`, `capacity`, or `shutdown`) and per-pool usage/seal counters.
Shutdown drops unpublished owner-private entries according to the existing
shutdown contract.

## Platform contract and tests

The fixed region must be reserved and mapped by the platform. In code-segment
mode, every allocation page transitions RX to RW through `enable_rw` before
JITLink writes. Executable pages return RW to RX through `enable_ex` at commit;
data/GOT pages intentionally remain non-executable and writable as required by
the existing split-segment contract.
`split_2m_to_4k` is performed independently for each pool. A pool never spills
into another cell or into public; exhaustion is an AOT fallback.

Host tests use injected alloc/seal/split callbacks and the real JITLink memory
manager layout/finalize path. Target validation must additionally exercise
interleaved cell requests, no-cell requests, malformed metadata, per-pool
failure, explicit retry, generation/shutdown cleanup, and 20 or more (ideally
150) linked versions. Physical page compactness is a platform result; the
tests distinguish semantic pool/address ordering from page-level layout.
