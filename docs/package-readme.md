# MedSignal Workbench 1.2.0

双击 `MedSignalWorkbench.exe` 即可打开随包提供的 PhysioNet BIDMC 第 01 段真实记录；下拉列表还包含低覆盖的第 05 段，便于检查未给出估计的窗口。无需安装 Qt、编译器或 Python，也无需联网。

界面提供 ECG、PPG、呼吸波形、两位人工标注者的呼吸标记及原监护仪数值；支持播放、暂停、多倍速、逐窗口审阅及 JSON / CSV 导出。数值缺失时显示 `—`。在 Window review 页筛选或排序后，双击一行可查看该完整 30 秒区间。

完整数据获取与源码：[GitHub 项目](https://github.com/3086707459-ai/SmartAIIndustrialMonitor)。

自带基线命令工具可在终端使用：

```powershell
./bidmc_benchmark.exe --data ./datasets/bidmc-demo --require-records 2 --output ./bidmc-demo-evaluation.json --csv ./bidmc-demo-windows.csv
```

将输出写到可写目录；部署验收时请将证据放在包目录之外。

原始数据的来源、作者、许可及校验值保存在 `datasets/README.md`、`datasets/bidmc-demo/README.md`、`datasets/bidmc-demo/LICENSE`。完整真实数据评测口径见 `docs/medical-validation.md`。

这是历史医疗数据研究回放程序。当前基线估计阻抗呼吸波形的周期，没有训练临床模型；不用于实时患者监护或临床决策。
