#!/usr/bin/env bash
# XBBR reproducibility gate (SPEC §9.3): identical (source + profile + flags)
# must produce bitwise-identical binaries across two independent links.
#
# Compiles the source once (clang output is deterministic for fixed input),
# links twice with ld.lld under --bb-cross-reorder, and compares the SHA256 of
# the two executables. Exits 0 iff they match. This is the CI gate that backs
# SPEC §9.3's reproducibility requirement on every (arch × output-type × mode)
# triple XBBR supports.
#
# Usage: xbbr-repro-check.sh <src.ll> <triple> <mode> [entry] [extra ld flags...]
#   src.ll   LLVM IR source (must define the entry symbol)
#   triple   clang -target triple (e.g. aarch64-linux-gnu, armv7a-linux-gnueabi,
#            thumbv7a-linux-gnueabi, x86_64-linux-gnu)
#   mode     partial | full   (the XBBR-active lld modes; `function`/`none` are
#            clang-side "XBBR off" and are the non-XBBR baseline, whose
#            reproducibility is lld's normal guarantee, not this gate)
#   entry    entry symbol (default: a)
#   extra    additional ld.lld flags (e.g. -pie, -shared, --defsym big=...)
# Env:  CLANG, LD_LLD  (default <repo>/build/bin/{clang,ld.lld})
#
# Example:
#   xbbr-repro-check.sh smoke.ll aarch64-linux-gnu full
#   xbbr-repro-check.sh pie.ll aarch64-linux-gnu full a -pie
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CLANG="${CLANG:-$REPO/build/bin/clang}"
LD_LLD="${LD_LLD:-$REPO/build/bin/ld.lld}"

SRC="${1:?usage: $0 <src.ll> <triple> <mode> [entry] [ld flags...]}"
TRIPLE="${2:?missing triple}"
MODE="${3:?missing mode (partial|full)}"
shift 3
# Optional 4th positional is the entry symbol — but only when it is NOT an ld
# flag (doesn't start with '-'). This lets `... full` use the default entry `a`
# while `... full -pie` treats -pie as an ld flag.
ENTRY="a"
LD_FLAGS=()
if [[ $# -gt 0 && "${1:0:1}" != "-" ]]; then
  ENTRY="$1"
  shift
fi
LD_FLAGS=("$@")

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$CLANG" -target "$TRIPLE" -O2 -fbb-cross-reorder="$MODE" -x ir "$SRC" \
  -c -o "$TMP/a.o" 2>/dev/null

# Two independent XBBR links.
"$LD_LLD" -e "$ENTRY" "$TMP/a.o" --bb-cross-reorder=foo \
  --bb-cross-reorder-mode="$MODE" --unresolved-symbols=ignore-all \
  "${LD_FLAGS[@]}" -o "$TMP/out1" 2>/dev/null
"$LD_LLD" -e "$ENTRY" "$TMP/a.o" --bb-cross-reorder=foo \
  --bb-cross-reorder-mode="$MODE" --unresolved-symbols=ignore-all \
  "${LD_FLAGS[@]}" -o "$TMP/out2" 2>/dev/null

H1="$(sha256sum "$TMP/out1" | awk '{print $1}')"
H2="$(sha256sum "$TMP/out2" | awk '{print $1}')"

if [[ "$H1" == "$H2" ]]; then
  echo "XBBR repro OK   triple=$TRIPLE mode=$MODE sha256=$H1"
  exit 0
else
  echo "XBBR repro FAIL triple=$TRIPLE mode=$MODE" >&2
  echo "  out1=$H1" >&2
  echo "  out2=$H2" >&2
  cmp "$TMP/out1" "$TMP/out2" >&2 || true
  exit 1
fi
