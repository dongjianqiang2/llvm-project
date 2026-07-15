---
name: ejit-pgo-p0-test-build
description: How to build standalone PGO P0 verification tests against the LLVM build (llvm-config recipe + aarch64 -O1 RA-bug workaround)
metadata: 
  node_type: memory
  type: project
  originSessionId: 1d6aa49c-891f-48b8-bd89-9f5121f73188
---

EJIT 在线 PGO 的 P0 验证用 `/tmp` 下独立 `main()` 程序(如 `pgo_p0_test.cpp`、`pgo_p0_6_test.cpp`),复制 LLVM pass 序列直接跑,不进 gtest。设计文档(`jit_design_doc/EJIT_ONLINE_PGO.md` §9)把它们当 P0 产物,后续才提升为正式 gtest。

构建配方(机器 aarch64,只有 `build_release_aarch64`):
```bash
cd build_release_aarch64
./bin/clang++ -O1 $(./bin/llvm-config --cxxflags) /tmp/pgo_p0_X_test.cpp -o /tmp/pgo_p0_X_test \
  $(./bin/llvm-config --ldflags --libs <components> --system-libs) -lpthread -ldl -lm
```
`--cxxflags` 自带 `-DEJIT_FREESTANDING -fno-rtti -fno-exceptions -std=c++17`。components 按需:`core passes analysis ipo instcombine scalaropts transformutils bitwriter support`(用到的 pass 对应组件;PGO 闭环另需 `instrumentation profiledata`)。`llvm-config --libs <components>` 自动展开传递依赖。

**Gotcha**:必须加 `-O1`。`-O0` 默认下编译这种 IRBuilder 密集的 TU 会撞 aarch64 后端 RA bug:"Cannot scavenge register without an emergency spill slot!"(P0-6 触发,P0 更小的 TU 没触发)。`-O1` 即可绕过。

设计文档当前版本 v0.5,P0-6 实测结论:激进内联(去 `buildModuleInlinerPipeline`)对小/可折叠 callee 是 Flash 代价 +14%~22%,仅中等 callee 多调用点场景省 −5.8%--推翻了"非内联版常 ≤ 内联版"的假设。详见 [[ejit-pgo-online-status]]。
