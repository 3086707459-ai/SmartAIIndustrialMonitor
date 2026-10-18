# Windows 自动验收

工作流 [windows.yml](../.github/workflows/windows.yml) 在推送 `main` 或提交 PR 时运行；也可从 GitHub Actions 手动启动，选择 60 秒或 1800 秒压力测试。默认短测用于及时发现回归，不能代替 30 分钟耐久验收。

执行环境为 `windows-2022`、Qt 6.8.3、MinGW 13.1、Release，关闭可选 ONNX / OpenCV，启用警告视为错误。Action 使用固定提交哈希，仓库权限为只读，不需要用户设置密钥。Qt 安装参数参照 [install-qt-action 文档](https://github.com/jurplel/install-qt-action)。

`scripts/ci.ps1` 顺序执行：

1. CMake 配置与完整编译。
2. 全部 CTest 目标，输出 JUnit XML。
3. 独立 TCP 网关压力测试，核对队列、损失与数据库行数。
4. 生成部署包，并在剥离 Qt SDK 环境变量的独立进程中验证原生 GUI、TCP、SQLite 与 DLL 来源。

任一步失败则任务失败；工作流仍尝试上传已有日志与证据，保留 14 天。数据库和测试程序不作为证据上传。部署探针仍在同一个装过 SDK 的 runner 上执行，**不等同于没有开发环境的干净 Windows 验收**。

本机复现（需要 PowerShell 7、CMake、Ninja；输出目录必须不存在）：

```powershell
./scripts/ci.ps1 -QtRoot E:/qt/6.8.3/mingw_64 -CompilerBin E:/qt/Tools/mingw1310_64/bin
```

CI 是否在 GitHub 成功应以对应提交的 Actions 状态为准。本机运行脚本通过不能记作远程 CI 通过。

2026-10-18 已在本机完整执行该脚本：严格 Release 构建、10 个 CTest 目标（77 个用例）、60 秒压力、部署打包与原生 GUI 隔离验收全部通过。运行记录保存在 `build/acceptance/ci-20261018`，包为 `build/packages/ci.zip`。首次试跑发现强制修改编译器路径会使 CMake 重置缓存，已改为通过 `CXX` 为全新构建选择编译器，并检查缓存中关键选项；失败试跑记录保留在 `build/acceptance/ci`，不计入通过证据。

随源码保存 [本机验收摘要](acceptance/ci-local-20261018.json) 与 [JUnit 记录](acceptance/regression-20261018.xml)。
