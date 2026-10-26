> 历史结果；对应旧工业源码与脚本。当前医疗版打包步骤见 [部署说明](deployment-acceptance.md)，不可用新版探针复现旧包结论。

# Windows 部署验收（2026-10-15）

## 已实测结果

修复后包：`build/packages/SAIM-demo-20261015-r2.zip`。

- SHA256：`271D0E1EDF412F3FC24261E7AF1B15B815BF783F82C5D0D30A9DDFAEDD9A33A6`。
- Release / Qt 6.8.3 / MinGW 13.1；ONNX 和 OpenCV 关闭。
- 本机 Windows 10 22H2，复制到临时目录运行，PATH 仅含 Windows 系统目录；清除 Qt、QML、SAIM、CMake 等环境覆盖。
- 包内 40 个文件通过清单哈希校验；实际加载的 Qt、MinGW、qwindows、qsqlite、SVG 插件均来自临时运行包目录。
- 原程序 `--version` 启动成功；探针使用同一 ApplicationController 创建原生 Windows 窗口，运行真实 loopback TCP、推理、Charts 和 SQLite。
- 实测 498 条 SensorData、30 个曲线点，数据库健康检查通过，窗口析构通过，SVG 渲染像素校验通过。
- 证据：`build/acceptance/package-20261015-r2/acceptance-result.json`、`probe-result.json`、`native-window.png` 和进程输出。
- 可随源码保存的摘要：[package-20261015.json](acceptance/package-20261015.json)，省略本机临时目录名称，完整原始证据保留在上述 build 目录。

这证明本机**进程级依赖隔离**通过。当前未检测到 Windows Sandbox 可执行程序，也没有可调用的虚拟机入口；**尚未完成全新 Windows 虚机或另一台电脑验收**。不能把 PATH 清理等同于操作系统干净环境，更不能据此承诺所有显卡驱动／Windows 版本兼容。

## 复现打包和本机隔离验收

在项目根目录执行，每次选择新的输出目录：

```powershell
./scripts/package.ps1 -QtRoot E:/qt/6.8.3/mingw_64 -CompilerBin E:/qt/Tools/mingw1310_64/bin -OutputDirectory build/packages/my-demo
./scripts/verify-package.ps1 -PackageDirectory build/packages/my-demo -EvidenceDirectory build/acceptance/my-demo
```

包包含运行程序、专用验收探针、Qt / 编译器运行库、平台和 SQL 插件、配置、`qt.conf`、`package-info.json`、SHA256 清单及包内验收脚本。`-SkipBuild` 仅供已明确完成当前源码构建时使用。

验收脚本检查文件缺失、额外文件、哈希变化和文件系统链接；在临时运行目录验证，证据保留在包外。包被视为不可变输入，不应在包目录中存放数据库或临时验收结果。执行超时或非零退出码即失败。

## 干净 Windows 验收入口

在已启用 Windows Sandbox 的主机，将包和项目脚本复制过去后生成配置：

```powershell
./scripts/create-sandbox-config.ps1 -PackageDirectory C:/Test/SAIM-demo -EvidenceDirectory C:/Test/evidence -OutputFile C:/Test/SAIM.wsb
```

打开生成的 `.wsb`：只读映射包、可写映射证据目录，关闭外网和剪贴板；沙盒内脚本运行 loopback 验收。工具不会自动安装或启用系统功能。

也可在全新 Windows 虚机中解压包，不安装 Qt、MinGW 或开发工具，执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./acceptance/verify-in-sandbox.ps1 -PackageDirectory . -EvidenceDirectory C:/SAIM-evidence
```

保留上述脚本产生的 JSON、截图和日志，并另行记录虚机镜像来源、版本、架构和确实未安装 SDK 的环境说明。只有这一步真实通过，才能把验收状态升级为“干净 Windows 环境通过”。

## 此次验收找到的缺陷

首次验收失败：`resolvePath()` 会搜索已有父目录，相对输出 `acceptance.db` 被写入指定数据目录的上一层。新增 `resolveDataPath()` 专门解析写入路径，数据库、日志和报告输出均以 data root 为基准；资源／模型查找仍使用原读取规则。增加真实建库回归测试，验证指定子目录中的新数据库不会逃逸到父目录。

`windeployqt` 提示未发现 `dxcompiler.dll / dxil.dll`。当前 Widgets / Charts 路径在原生平台验收通过；本次不涉及 QML、D3D12 或真实 ONNX / OpenCV 后端，这些部署组合需要各自的测试包。
