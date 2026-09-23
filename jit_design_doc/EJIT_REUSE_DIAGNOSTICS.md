# Why two requests did not share code

These diagnostics supplement PR230's exact identity checks. They do not relax
those checks, force a merge, or count a normal unequal-value split as a failure.
They run on classification/compilation paths, never per cache-hit execution.
Build with `EJIT_DIAG_ENABLE` and the existing representative-sharing options;
keep `EJIT_SRE_PGO_BRANCH_AUDIT=ON` for validation.

## Board usage

No runtime preconfiguration is needed: detailed capture defaults to all entries.
Run the existing workload, then on worker core 6 inspect retained records with:

```text
ejit_reuse_diag_print
```

The C APIs are `ejit_reuse_diag_config(const char *entry, uint32_t level)`,
`ejit_reuse_diag_print(void)` and `ejit_reuse_diag_reset(void)`. Use your shell's
normal string/argument syntax. These calls must run on the actual compile-owner
core after initialization; a peer call returns `EJIT_ERR_NOT_ACTIVE`, not an
apparently empty private registry. They do not route through a cross-core
mailbox. `EJIT_PENDING` means a diagnostic operation was busy: retry later.

Level 0 disables new capture, 1 records summaries, and 2 adds paired
first-difference excerpts (default, filter `*`). Automatic logs print summaries
only; the explicit print command also prints retained excerpts. The filter is an exact function name or
`*`, up to 95 printable ASCII bytes; an empty filter is equivalent to `*`.
Configuration clears the old capture window. Reset clears records but preserves
the filter/level. In-flight compilation can straddle config/reset. Turning on
level 2 after explicitly using level 0/1 cannot recover uncaptured historical IR;
enable it BEFORE a fresh run in that case. A cached existing candidate is not reclassified
just to populate diagnostics.

If unrelated events evict the record of interest, optionally use
`ejit_reuse_diag_config "reuse_0", 2` before a focused rerun. It is not required
for the first test run. Shutdown restores the default all-entry detailed capture.

## Reading the record

Illustrative shape (values depend on the module):

```text
[REUSE_DIAG] seq=1 entry=reuse_0 func=0 stage=CANDIDATE reason=PREFIX_IR_DIFF action=NEW_GROUP generation=1 attempt=7 group=2 group_gen=0 peer_group=1 peer_code=0 repeats=0 truncated=1 detail=...
[REUSE_DIAG] seq=1 dim=0 instance=5 version=1
[REUSE_DIAG] seq=1 first_diff_line=12 byte=420 left(peer)=...
[REUSE_DIAG] seq=1 right(request)=...
```

`entry`, `func`, dimensions/instance/version and attempt identify the request.
Candidate `group`/`peer_group` are directory candidate IDs; `group_gen=0` means
no runtime sampling generation is being claimed at this classification point.
For a new candidate, the diagnostic peer is the lowest-ID retained candidate
with the same entry and original bitcode digest, even across hash buckets. It
is a deterministic reference, NOT a claim that every other group was compared
in the log. The classifier still performs its unchanged exact-match search.
At FINAL, `group` and `group_gen` identify the actual live group and `peer_code`
is the group's retained physical code object.

- `NEW_GROUP`: normal candidate split. Different gain values can produce a
  `PREFIX_IR_DIFF`; this is not a broken compiler or an incorrect mayconst.
- `TRY_SEPARATE_CODE`: final identity differs from the group's physical code;
  independent emission is attempted. This line is not a publication certificate.
- `ORDINARY_ROUTE` / `AOT_FALLBACK`: the sharing path was unavailable/rejected;
  inspect the reason and detail (`TARGET_MISMATCH`, `PREPARE_REJECTED`,
  `LINK_FAILED`, `NO_EMITTER`, etc.). This can require a runtime/configuration fix.

Differences distinguish source/policy/entry scope, binding symbol/address/kind,
profile schema fields, and canonical IR. Left is the retained peer identity;
right is the incoming request. IR line/byte offsets refer to CANONICAL IR, not
the C source line. A constant may already be folded into an expression:
the IR excerpt alone cannot reconstruct original mayconst values or names.
The separate replacement records below provide frozen values when available.
For bindings, field/name and actual compared values are available directly.
Only the first IR difference is reported; frozen-value differences have their
own bounded multi-record output.

## Bounds and safety

### Frozen mayconst values (candidate splits)

Candidate preparation now copies the actual scalar constant at the load
replacement point. It does NOT read business memory again for diagnostics.
When two candidate groups split, the default capture also compares their
recorded substitutions. The existing IR reason and reuse decision are unchanged.
For example, explicit `ejit_reuse_diag_print` can add:

```text
[REUSE_DIAG] seq=1 frozen_available=1 frozen_compared=2 frozen_different=2 shown=2 incomplete=0 (recorded substitutions, not verifier)
[REUSE_DIAG] seq=1 MAYCONST_VALUE_DIFF site=1 origin=f:rows+field_offset=0 peer_frozen=i32 3 request_frozen=i32 17
[REUSE_DIAG] seq=1 MAYCONST_VALUE_DIFF site=2 origin=f:rows+field_offset=4 peer_frozen=i32 5 request_frozen=i32 9
```

Use `seq` to join these lines to the entry/dimensions/version and `peer_group`.
The peer is the oldest retained same-entry/source candidate, possibly from an
earlier lifecycle, NOT necessarily the current equal-value group. A frozen-value
difference accompanies the IR split; it does not prove it was the only cause.
Equal recorded values do not prove identical IR, and this is not the verifier's
comparison of frozen versus actual execution-time values.

Sites are input-load ordinals assigned before specialization, scoped to the
same source module. They are not C line numbers. Origin is the input function,
base symbol when recoverable, and field offset when recoverable; field names
and source lines are not promised. Cloned/repeated sites, lost metadata, missing
records, unsupported constants or truncated text produce incomplete diagnostics,
not an invented correspondence. A transformed-away load may never reach the
replacement pass; comparison describes recorded replacements, not every
mayconst in the source.

Each snapshot retains up to 32 substitutions (64-byte origin and 48-byte typed
value buffers), with at most 128 reference snapshots per candidate directory,
roughly 0.5 MiB payload maximum on 64-bit builds. These are separate from the
identity admission budget. Input tagging is capped at 4096 loads; replacement
sites without tags count as omitted. No LLVM/business pointers are retained.
Each diagnostic stores up to four differing pairs but reports the total
differences found among comparable records; `different > shown` means output
was capped. `incomplete=1` means some comparisons could not be made;
`frozen_available=0` means paired history is unavailable. Neither means equal.

Automatic logs print counts only; level 2 explicit print shows values. These
records are available for candidate splits, not arbitrary final-only IR changes.
Reference snapshots follow the candidate directory lifetime; shell reset/config
only clears the event window, not this compiler-owned history. Snapshot capture
is independent of the output level so future splits can still be explained;
level 0 disables events, not this bounded provenance collection.
Diagnostic tags are stripped before PGO schema generation and from canonical
identity. No additional sample, verifier behavior, code-identity rule, or shared
taskpool ABI change is introduced.

### Event retention

There are 16 fixed-size owner-local retained records. Oldest records are evicted
when full. The same function/generation/dimensions/versions/group/peer/stage/reason/action is logged
once while retained; later events increment `repeats` and preserve the first
request identity. After eviction it can be logged again. `evicted`, `dropped`
(nonblocking contention) and `truncated` are explicit; an empty log is NOT proof
that all requests shared. At most 256 bytes per IR side and 192 bytes of detail
are retained per record; names/reasons also have fixed limits. Control characters
are replaced with spaces for one-line logs. Select one function to avoid unrelated
events evicting the interesting one. Addresses and IR may reveal application
details: restrict access to diagnostic output. Level 0/1 limits event output;
it is not a security switch to disable compiler-owned frozen-value history.

Existing identity material is reused to calculate the difference. No extra full
module/IR copy is retained, no worker registry pointer escapes, and no shared
taskpool ABI is changed. Shell snapshot/capture uses a single try-lock: busy
diagnostics can be lost instead of waiting for that lock, and comparison results
never change a sharing decision. Printing occurs after releasing the lock;
synchronous board logging and extra comparisons still cost time and can perturb
scheduling, so avoid broad detailed capture in latency-sensitive production.
Existing general EJIT logs are not silenced by this filter. This is a bounded first-diff
debugger, not an unlimited full-IR archive or a complete mayconst provenance map.
