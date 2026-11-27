# -*- coding: utf-8 -*-
"""Measured numbers used across the deck (single place to update).

Provenance: HEAD 536b4bbff925, release x86 build rebuilt 2026-11-27, host = Intel Xeon Gold 5120
(shared server). Kernels in bench/demo.c (this directory); official = ejit_test/ejit_perf_bench.c.
Host microbenchmarks, NOT board numbers.
"""

M = dict(
    build='HEAD 536b4bbff925 · 2026-11-27 · x86-64 Xeon Gold 5120（共享服务器）',
    # stream kernel, ns per sample (median of 9 reps, 3 runs agree within 2%)
    stream_aot_ns=3.97, stream_jit_ns=0.94,
    # tiny entry, ns per call
    tiny_pure_ns=5.4, tiny_hit1_ns=16.6, tiny_hit16_ns=21.8, tiny_mixed_ns=62.4, tiny_miss_ns=69.0,
    # official ejit_perf_bench.c (20M iterations)
    micro_aot_ms=93, micro_aot_ms_hi=104, micro_jit_us=0.06,
    official_speedup='474,369.6', official_speedup_num=474369.6, official_speedup_runs='47 万 · 49 万 · 143 万',
    ip_aot_ms=133, ip_jit_ms=333,
    # enqueue -> published, ms
    lat_first_ms=(14, 16), lat_warm_ms=(6.3, 6.7), lat_tiny_ms=(3.4, 4.0),
    # tests at HEAD
    gtest='272 + 2 skipped / 52 / 169', lit='55 / 55 · 28 / 28', it='27 过 · 2 红 · 2 未能构建',
    # sizes
    jit_bytes=dict(compute_cell=6, tiny=16, stream_kernel=55),
)
