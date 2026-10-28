# 医疗版 Windows 部署验收

当前分发入口为 `MedSignalWorkbench.exe`。1.2 包包含原生 Qt 界面、BIDMC 01 与 05 原始三类 CSV、署名及原始许可、发布方哈希清单、`bidmc_benchmark.exe`、部署探针、Qt / MinGW 运行库和验收脚本。

```powershell
./scripts/package.ps1 -QtRoot E:/qt/6.8.3/mingw_64 -CompilerBin E:/qt/Tools/mingw1310_64/bin -OutputDirectory build/packages/medical-demo
./scripts/verify-package.ps1 -PackageDirectory build/packages/medical-demo -EvidenceDirectory build/acceptance/medical-demo
```

输出目录须不存在。包内清单验证缺失、额外文件、哈希变更和重解析链接；验收复制到临时目录，清除 Qt / SDK 环境覆盖，PATH 仅保留 Windows 系统目录，使用原生 qwindows 平台。

## 必须通过的条件

- 实际入口 `--version` 能启动。
- 同一 `MedicalWindow` 从包内读取原样第 01 段，原始哈希匹配、60,001 行波形和 481 行数值。
- 定位 111 秒时缺失 SpO2 显示 `—`；定位 30 秒时三通道共 3,753 个原始点被绘制。
- 呼吸基线输出 16 个窗口，JSON 可写入包外证据目录。
- 原生窗口可见、SVG 像素验证通过、Qt / 编译器 DLL 来自临时运行包；Qt Core、Charts、Svg 与 qwindows 必须有加载证据。当前使用的 QtConcurrent 模板由 QtCore 线程池支撑，不要求单独加载 Qt6Concurrent.dll。
- 窗口审阅表包含 16 行，导航至完整 30 秒区间时每通道绘制 3,750 个点；CSV 导出可读。
- 原样第 05 段能够离线加载，16 个窗口中保留 2 个可计分窗口和 14 个拒绝估计窗口。

证据包括 `acceptance-result.json`、`probe-result.json`、`bidmc-demo-evaluation.json` / `.csv`、`native-window.png`、`native-review.png` 及进程日志。医疗版 1.2 结果详见 [验收摘要](acceptance/medical-package-20261028.json)；[1.1 历史结果](acceptance/medical-package-20261026.json) 单独保留。

## 干净 Windows 的额外步骤

上述验收是本机或 CI 的进程级依赖隔离。尚未完成没有 SDK 的全新虚拟机／第二台电脑验收。

在已启用 Windows Sandbox 的主机上可生成配置，脚本不自动启用系统功能：

```powershell
./scripts/create-sandbox-config.ps1 -PackageDirectory C:/Test/MedSignal -EvidenceDirectory C:/Test/evidence -OutputFile C:/Test/MedSignal.wsb
```

也可在无 Qt / MinGW 的全新 Windows 虚机中解压后执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./acceptance/verify-in-sandbox.ps1 -PackageDirectory . -EvidenceDirectory C:/MedSignal-evidence
```

保留镜像来源、系统版本、未安装 SDK 的环境说明以及所有原始证据，才能确认干净 Windows 环境通过。

2026-10-15 工业原型的 TCP / SQLite / GUI 验收保留在 [历史记录](deployment-history-20261015.md)，不作为本医疗版的测试结果。
