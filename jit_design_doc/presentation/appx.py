# -*- coding: utf-8 -*-
"""附录（总览里可见）"""
from svgkit import code

ACT = 6


def a1_api():
    rows = [
        ('初始化', '<code>ejit_init(cfg)</code> · <code>ejit_init_pgo(cfg)</code> · <code>ejit_shutdown()</code> · <code>ejit_default_config</code>（测试辅助）'),
        ('生命周期', '<code>ejit_activate(name, idx)</code> · <code>ejit_deactivate</code> · <code>ejit_activate_all</code> · <code>ejit_deactivate_all</code> · <code>ejit_is_active</code>'),
        ('编译 / 查询', '<code>ejit_taskpool_compile_or_get(funcIndex, dims, n, &amp;fn, &amp;bucket)</code> · <code>_bound</code> · <code>_bound_v</code> · <code>ejit_taskpool_release_read(bucket)</code> · <code>ejit_publish_pending_code()</code>'),
        ('注册（AOT 生成代码调用）', '<code>ejit_register_symbol</code> · <code>ejit_register_lifecycle</code> · <code>ejit_register_funcindex</code> · <code>ejit_register_icache_slot</code> · <code>ejit_register_pointer_period_bounds</code> · <code>ejit_set_allocation_size_lookup</code>'),
        ('观测', '<code>ejit_taskpool_get_stats</code> · <code>ejit_get_code_pool_stats(_v2)</code> · <code>ejit_print_period_const_ranking</code> · <code>ejit_print_active</code> · <code>ejit_dump_func</code> / <code>ejit_print_dumped</code> · <code>ejit_set_log_level</code> · <code>ejit_print_version</code>'),
        ('上限', 'lifecycle 8 个 · 实例 256 个 · funcIndex 4096 · <code>ejit_dim</code> ≤ 4 个/函数 · <code>EJIT_BOUND_PTR</code> ≤ 8 个/入口 · 队列 1024 · 桶 32 × 16 槽'),
    ]
    trs = ''.join(f'<tr><td style="width:20%"><b>{a}</b></td><td>{b}</td></tr>' for a, b in rows)
    body = f'''<table class="tbl air" style="font-size:16.5px"><tbody>{trs}</tbody></table>
<div class="grid g3" style="gap:16px;margin-top:18px">
  <div class="card pad-sm rule-ice"><div class="card-h">用户宏 · 7 个</div><p class="small" style="color:var(--ink-2)"><code>EJIT_PERIOD_CONST</code> <code>EJIT_GROUP(x)</code> <code>EJIT_DIM(x)</code> <code>EJIT_GUARD(x)</code> <code>EJIT_ENTRY</code> <code>EJIT_FREE_DIM</code> <code>EJIT_BOUND_PTR(x)</code></p></div>
  <div class="card pad-sm rule-violet"><div class="card-h">编译模式 · 只剩 2 个</div><p class="small" style="color:var(--ink-2)"><code>EJIT_COMPILE_ASYNC = 1</code> · <code>EJIT_COMPILE_OFF = 2</code>。0 号是已退役的 Sync；全零 config 会被拒绝。</p></div>
  <div class="card pad-sm rule-amber"><div class="card-h">43 个导出函数</div><p class="small" style="color:var(--ink-2)">其中 12 个是诊断打印。<code>ejit_get_stats</code> 是桩，恒返回 0；真实计数在 <code>ejit_taskpool_stats_t</code>。</p></div>
</div>'''
    return dict(act=ACT, name='api', kicker='附录 A1', title='公开 C API 速查', body=body, notes='<p>Q&A 时备查。全部在 <code>llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h</code>。</p>')


def a2_method():
    left = code('''# 工具链：HEAD 536b4bbff925 · build_release_x86
#   Release · LLVM_ENABLE_THREADS=OFF
#   SHARED_TASKPOOL / SHARED_CODE_POINTERS / STATS = ON
#   -ejit-inline-cache 默认开
ninja -C build_release_x86 clang lld opt LLVMEJIT

# 官方微基准
clang -O2 -c ejit_test/ejit_perf_bench.c
clang++ -fuse-ld=ld.lld -Os -Wl,--gc-sections \\
  -Wl,--whole-archive libLLVMEJIT.a \\
  -Wl,--no-whole-archive <LLVM .a ...> perf_bench.o
./ejit_perf_bench 3 5

# 测试套件
./bin/EJITTests; ./bin/EJITCodePoolTests
./bin/EJITSharedTaskPoolTests
./bin/llvm-lit ../llvm/test/Transforms/EmbeddedJIT
./bin/llvm-lit ../clang/test/CodeGen/ejit_*.c \\
  ../clang/test/Sema/ejit_*

# 幻灯片里的 AArch64 汇编（目标板形态）
bench/build.sh aot-a64              # AOT：-emit-llvm → aarch64 llc -O2
EJIT_DUMP_DIR=/tmp/ejd bench/out/demo    # JIT：运行时导出特化后 IR
$ROOT/build_aarch64/bin/llc -O2 -mtriple=aarch64-linux-gnu /tmp/ejd/*_opt.ll''', 'sh', 'xs', maxw=640, cap='怎么测的')
    right = '''
<div class="card pad-lg rule-lime" style="margin-bottom:14px"><div class="card-h">口径</div>
<ul class="ul">
  <li class="li tight">主机：Intel Xeon Gold 5120 @ 2.20 GHz，56 逻辑核，<b>共享服务器</b>（测试期间 load 4–18）</li>
  <li class="li tight">计时：<code>clock_gettime(CLOCK_MONOTONIC)</code>，9 次取中位数；3 次独立运行相差 ≤ 2%</li>
  <li class="li tight">stream 内核：4096 样本 × 4000 轮；tiny：2000 万次调用</li>
  <li class="li tight">幻灯片汇编：全部是 AArch64 后端的真实输出 —— AOT 走 <code>clang -O2</code>，JIT 走运行时导出的特化后 IR（第 6、17、27、29 页）</li>
</ul></div>
<div class="card pad-lg rule-red"><div class="card-h">这些数字<b>不能</b>说明什么</div>
<ul class="ul">
  <li class="li tight">不是板端数据。aarch64_be / SRE 上的收益基线至今没有</li>
  <li class="li tight">没有 PMU：<code>perf_event_paranoid = 4</code>，x86 上探针多出的 11 ns 原因未证实</li>
  <li class="li tight">全是微基准；共享机器有噪声</li>
  <li class="li tight">乱序大核会掩盖 load / 分支的代价，顺序嵌入式核上差距应更大 —— 但这是推测</li>
</ul></div>'''
    body = f'<div class="grid g-55-45" style="gap:28px"><div data-anim="left">{left}</div><div data-anim="right">{right}</div></div>'
    return dict(act=ACT, name='method', kicker='附录 A2', title='数字的出处与测量方法', body=body,
                notes='<p>Q&A 时如果有人问"这个数怎么来的"，翻到这页。</p>')


def a3_glossary():
    L = [('AOT', 'Ahead-of-Time，提前编译'), ('bitcode', 'LLVM IR 的二进制格式；被嵌进可执行文件'), ('时间窗 / 窗口', '一段数据保持不变的时间；由 activate / deactivate 界定'),
         ('period_const', '窗口内可视为常量的字段（EJIT_PERIOD_CONST）'), ('ejit_group', '全局变量所属的时间窗；"static" 永远激活'), ('ejit_dim', '特化维度：时间窗数组的下标'),
         ('EJIT_ENTRY', '值得特化的函数入口'), ('EJIT_GUARD', '会修改窗口数据的函数；前后插 deactivate / activate'), ('wrapper', '编译器插在 EJIT_ENTRY 入口的探针代码'),
         ('inline cache / cell 表', '每个 entry 一张 [16]^维度 的函数指针表；命中即跳'), ('worker', '唯一的编译线程 / 核'), ('taskpool', '队列 + 缓存 + 去重的总称'),
         ('桶 / read-token', '32 个分桶各一把读写锁；读者拿到令牌，用完归还'), ('version', '每个实例一个单调递增的版本号；toggle 即失效'),
         ('外链化', 'bitcode 瘦身：≥16 条指令的非入口函数只留声明，运行时解析到 AOT 定义'), ('W^X / 封固', '写完的代码页从 RW 改成 RX，不可同时可写可执行'),
         ('Tier-1 / Tier-2', '带计数器的特化版 / 用 profile 重编译的版本'), ('value profiling', '记录重复出现的值（调用目标、访存大小、循环边界）'),
         ('witness', 'PASS6 算地址时代入的代表值；只进地址运算，不进 IR'), ('SRE', '目标裸核实时环境（aarch64_be）'), ('deopt', '去优化：服务器 JIT 猜错后的退路；EJIT 没有')]
    R = [('EJIT_DISABLE', '定义后 7 个标注宏全部展开为空'), ('NO_RECLAIM', '不回收代码的构建：命中路径免读令牌，9 ns → 见 CQ 文档'), ('lipo', '把 53 个静态库压成一个 ejit.o 的三步流程'),
         ('owner', 'CAS 赢的那个核，负责跑 worker'), ('executableCoreMask', '64 位位图：哪些核已对某段代码页做过 enable_ex'), ('shared blob', 'EJIT_SHARED_SECTION 里的全 POD 共享状态'),
         ('funcIndex', '每个 entry 一个稠密下标（≤ 4096），init 时填好'), ('dimType', '每个生命周期一个槽位（≤ 8）'), ('dispatch / MissFn', '热路径直跳 / noinline 的慢路径函数'),
         ('commit gate', '发布前在桶写锁内重校验 version'), ('dedup', '同一个 (函数, 实例) 在队列里只留一份'), ('Vyukov 队列', '有界无锁 MPMC 环；EJIT 只用单消费者'),
         ('NEON', 'JIT 流水线在 AArch64 上打开的向量扩展'), ('MBP', 'MachineBlockPlacement，后端的基本块布局'), ('ICP', '间接调用提升：把间接调用改成带 guard 的直接调用')]
    half = (len(L) + 1) // 2
    trs = ''.join(
        f'<tr><td style="width:15%"><code>{a}</code></td><td style="width:35%">{b}</td><td style="width:15%"><code>{c}</code></td><td>{d}</td></tr>'
        for (a, b), (c, d) in zip(L, R))
    body = f'<table class="tbl tight" style="font-size:15px"><tbody>{trs}</tbody></table>'
    return dict(act=ACT, name='glossary', kicker='附录 A3', title='术语表', body=body, notes='<p>备查。</p>')


def a4_sources():
    rows = [
        ('AOT pass（5 个）', 'llvm/lib/Transforms/EmbeddedJIT/ —— EJitRegisterBitcode.cpp（1189 行）· EJitWrapperGen.cpp（1067）· EJitRegisterPeriod.cpp · EJitPeriodHandler.cpp · EJitAotModulePass.cpp'),
        ('接入点', 'llvm/lib/Passes/PassBuilderPipelines.cpp:1673–1686 —— PASS1 与 PASS2–4 都插在 O2/O3 之前'),
        ('运行时', 'llvm/lib/ExecutionEngine/EJIT/ —— EJitSharedTaskPool.cpp（3719）· EJitStructFieldPass.cpp（2510）· EJitOrcEngine.cpp（1312）· EJitOptimizer.cpp · EJitRuntime.cpp · EJitCodePool.cpp'),
        ('公开头', 'llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h（451 行）'),
        ('Clang 侧', 'clang/lib/Sema/SemaEJIT.cpp（1099）· clang/lib/CodeGen/CGEJIT.cpp（387）· clang/include/clang/Basic/Attr.td:5221–5282'),
        ('核心 LLVM 的"语义"改动', 'llvm/include/llvm/IR/FixedMetadataKinds.def:56（MD_ejit_period_const = 42）· llvm/lib/Transforms/Utils/Local.cpp:3497（copyMetadataForLoad）'),
        ('测试', 'llvm/unittests/ExecutionEngine/EJIT/（550 个 TEST 宏）· llvm/test/Transforms/EmbeddedJIT/（55）· clang/test/CodeGen|Sema/ejit_*（28）· ejit_test/（45 个 .c，31 个目标）'),
        ('设计文档', 'jit_design_doc/ —— SPEC4.md · PASS1–7 · EJIT_SRE_TASKPOOL.md · EJIT_SRE_CODE_POOL.md · EJIT_ONLINE_PGO.md · EJIT_CACHE_QUERY_PERF.md 等 30 余份'),
        ('真实产物', 'IR：EJIT_DUMP_DIR 运行时导出的 pre / opt（第 16 页）· 汇编：AOT clang -O2、JIT 特化后 IR，统一过 AArch64 后端 llc -O2 -mtriple=aarch64（第 6、17、27、29 页）'),
    ]
    trs = ''.join(f'<tr><td style="width:20%"><b>{a}</b></td><td class="mono" style="font-size:14.5px">{b}</td></tr>' for a, b in rows)
    body = f'<table class="tbl air"><tbody>{trs}</tbody></table>'
    return dict(act=ACT, name='sources', kicker='附录 A4', title='源码索引：今天看到的每样东西在哪', body=body, notes='<p>备查。</p>')


def slides():
    return [a1_api(), a2_method(), a3_glossary(), a4_sources()]
