# EJIT 演讲：《把运行时，变成编译期。》

成品是上一级目录的 **`EJIT_TALK.html`**：单文件（约 800 KB，字体子集内嵌，无外部请求），
直接用浏览器打开即可放映。本目录是它的生成器、分镜、基准测试和截图检查脚本。

```
python3 build.py            # 重新生成 ../EJIT_TALK.html
python3 build.py out.html   # 或指定输出
```

## 依赖

- Python 3.9+ 与 fontTools（`pip install fonttools brotli`；装在别处就用 `EJIT_TALK_PY=<目录>` 指过去）；`sheet.py` 另需 Pillow
- 字体（均为 OFL）放在 `EJIT_TALK_FONTS` 指向的目录（默认 `/tmp/pptools/fonts`），文件名需为：

| 文件 | 来源 |
|---|---|
| `NotoSansSC.ttf`（可变字重） | <https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf> |
| `NotoSerifSC.ttf`（可变字重） | <https://raw.githubusercontent.com/google/fonts/main/ofl/notoserifsc/NotoSerifSC%5Bwght%5D.ttf> |
| `JetBrainsMono-{Regular,Medium,Bold,ExtraBold}.ttf` | <https://github.com/JetBrains/JetBrainsMono/releases> |

`build.py` 只嵌入幻灯片实际用到的字形，中间产物写在 `.build/`（已 git-ignore）。
构建会报告缺字形和超宽的代码块（`<-- OVER`），两者都应为零。

## 目录

| 文件 | 作用 |
|---|---|
| `STORYBOARD.md` | 分镜：五幕的结构、"颜色 = 概念"的约定 |
| `act0.py` … `act5.py`、`appx.py` | 每一页一个 `sN_name()` 函数，按幕分文件 |
| `svgkit.py` | 构建 SVG / 代码块的助手：`svg` `T` `TL` `RECT` `arrow` `box` `lane` `code` |
| `data.py` | 全部实测数字（`M`），每张幻灯片只从这里取值 |
| `base.css` `fx.css` `deck.js` `live.js` | 版式与组件 / 转场与分步 / 放映引擎 / canvas 动画 |
| `build.py` | 拼装 HTML：包一层 `<section>`、合并 CSS/JS、子集化字体 |
| `splice.py` | 用另一个文件里的同名函数替换 `act*.py` 里的函数（批量改稿用） |
| `sheet.py` `qa/` | 截图拼版；`qa/shot.js` 逐页截图、`qa/measure.js` 版面越界审计、`qa/ui.js` 总览/备注/转场/打印/演讲者视图 |
| `bench/` | 演讲里数字的来源：`demo.c` `probe.c` 与构建脚本 |

## 改一页

每页函数返回一个 dict，`build.py` 把它包成 `<section class="slide">`：

- `kicker` / `title` / `sub` — 页眉；`body` — 正文 HTML（SVG 直接内联）
- `steps=N` — 分步数；正文里带 `data-step="k"` 的元素在第 k 步出现，`data-until="k"` 表示到第 k 步前可见
- `trans='rise'|'zoom'|'drop'|…` — 进入这一页的转场；跨幕自动用快门（shutter）
- `live='hero'|'flows'|'ring'|'pool'` — 挂到 `live.js` 里对应的 canvas 动画
- `notes` — 演讲者备注（HTML）
- `html=` + `cls='bleed'` — 整页自定义（封面、章节页）

约定见 `STORYBOARD.md`：amber = AOT/未知，ice = 常量/特化，violet = worker 核，red = 坏消息（永远配文字）。

## 放映

`←/→` 翻页与分步 · `O` 总览 · `N` 备注 · `P` 演讲者视图（新窗口，含计时） · `T` 深浅色 · `F` 全屏 ·
`B`/`W` 黑屏/白屏 · `M` 动效开关 · `?` 帮助 · `Ctrl+P` 导出 PDF（每页一张，自动展开全部分步）。
URL 的 `#12.3` 直接定位到第 12 页第 3 步。

## 数字从哪来

全部是 x86-64 主机（Xeon Gold 5120，共享服务器）上对 HEAD `536b4bbff925` 的 release 构建的微基准，
**不是板端数据**。复现：

```bash
ninja -C build_release_x86 clang lld opt LLVMEJIT      # 先有工具链
bench/build.sh            # 生成 bench/out/demo 与 bench/out/probe（静态链接 libLLVMEJIT.a）
bench/out/demo            # 输出 TIME 行：micro / ip / stream / tiny / LATENCY
bench/build.sh aot        # 只出 demo 的 AOT 汇编（第 6、17 页用的是它和 objdump 的真实输出）
```

`./out/demo mem` 会把 dump 出来的 `jit_*.bin` 机器码写到当前目录。

`demo.c` 里的 `TIME` 行对应 `data.py`：`stream_ms` → `stream_*_ns`，`tiny_ns` → `tiny_*_ns`，
`ip_ms` → `ip_*_ms`，`micro_ms` → `micro_*`，`LATENCY_ms` → `lat_*`。官方 benchmark 的
`474,369.6×` 来自 `ejit_test/ejit_perf_bench.c` 本身的打印。

已知环境限制：本机 `perf_event_paranoid=4`，拿不到 PMU 计数，所以第 29、30 页里与 cache 相关的
结论都标了"未证实"。三次运行之间数字有 ±2% 左右的漂移，共享服务器上属正常。

## 截图检查（可选）

需要 Node 与 Playwright（`npm i playwright && npx playwright install chromium`），脚本默认从
`playwright` 包加载，装在别处用 `PW_PATH=<包路径>` 指过去：

```bash
node qa/shot.js ../EJIT_TALK.html /tmp/shots all      # 每页最终状态；LIGHT=1 换浅色
node qa/shot.js ../EJIT_TALK.html /tmp/shots 12@2+800  # 第 12 页第 2 步，多等 800ms
node qa/measure.js ../EJIT_TALK.html                  # 逐页报告内容底边/右边（越界会标出）
python3 sheet.py /tmp/shots /tmp/sheets 6 3           # 拼成每张 6 页的对照图
```
