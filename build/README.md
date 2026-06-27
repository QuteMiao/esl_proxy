# esl_proxy 上板构建

构建 + 运行已合并为单一入口 `tools/run_onboard.sh`（内联原 `build_aicpu.sh` /
`build_aicore.sh` / `build_onboard_host.sh` 三个脚本的逻辑）：

```bash
source $ASCEND_HOME_PATH/bin/setenv.bash
bash tools/run_onboard.sh                 # 构建 + 运行
bash tools/run_onboard.sh --skip-build    # 仅运行（复用已有产物）
```

CMake 源目录：`build/cmake/{aicpu,aicore,host}/`  
构建产物：`build/onboard/{aicpu,aicore,host}/`

上板运行需经 `task-submit --device auto` 包装（独占设备）。
