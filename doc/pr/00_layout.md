# PR-A0：目录分层（无代码语义变更）

相对基线：`origin/main`。

---

## 1. 为什么修改

`main` 把算法头与实现平铺在 `include/`、`src/`，后续要叠 **platform（onboard / sim）** 时会和算法文件混在一起，难以按后端分流审查。

本 PR 做目录搬迁，为 PR-A 的 `platform/` 与三端构建铺路。

---

## 2. 怎么修改

| 动作 | 路径 |
|------|------|
| 搬迁 | `esl_proxy/include/*.h` → `esl_proxy/include/algorithm/` |
| 搬迁 | `esl_proxy/src/{cutter,dispatch,executor,log,manager,shm}.c` → `esl_proxy/src/algorithm/` |
| Makefile | `-I include` → `-I include/algorithm`；同步 `SRCS` / `OBJS` / `mkdir -p`；`test-tensormap` 的 `-I` |

---

## 3. 与 simpler 的对应

| esl_proxy（本 PR） | simpler（a2a3） |
|--------------------|-----------------|
| `include/algorithm/` + `src/algorithm/` | `simpler/src/a2a3/runtime/`（编排 / 调度 / tensormap 等算法侧） |
| （后续 PR 引入）`platform/` | `simpler/src/a2a3/platform/{onboard,sim,...}/` + `src/common/platform/` |
| 算法与平台分目录 | 文档：`simpler/src/a2a3/docs/platform.md`、`runtimes.md` |
