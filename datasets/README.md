# BIDMC 1.0.0：真实公开医疗数据

项目主场景为历史医疗生理信号回放与基线评测。原始数据入口：[PhysioNet BIDMC](https://physionet.org/content/bidmc/1.0.0/)，数据 DOI：[10.13026/C2208R](https://doi.org/10.13026/C2208R)。

来源为 Beth Israel Deaconess Medical Centre 住院期间采集的记录，经发布者从 MIMIC-II 提取、整理为 53 段八分钟记录。波形采样率 125 Hz，监护仪数值采样率 1 Hz；人工呼吸标注由两位标注者独立提供。

## 目录与完整性

- `bidmc-demo/`：原样保留的第 01 段 Signals、Numerics、Breaths CSV，以及发布方原始 LICENSE。用于离线运行、常规回归和分发验收。未截断、插值或修改幅度。
- `bidmc/`：下载工具生成的完整本地数据目录，已加入 `.gitignore`，不将约 153 MB 的完整 CSV 重复提交至 Git。
- `SHA256SUMS-bidmc-1.0.0.txt`：从发布方版本目录获取的完整 SHA-256 清单。下载器和评测程序均核对输入文件哈希。

```powershell
# Python 3.9+ 与 curl；没有第三方 Python 依赖
python scripts/fetch_bidmc.py --all --mirror
# 只下载一段时，省略 --all，默认 01；也可 --records 01 23
```

`--mirror` 使用官方 archive.physionet.org 镜像，仍须匹配 physionet.org 版本目录的哈希。下载器支持部分文件续传；哈希不匹配即失败。脚本只获取这三个 CSV，不获取固定人口学变量。`download-manifest.json` 保存每个文件的真实下载 URL、大小与校验值。

## 许可与署名

PhysioNet 版本页面列示 **Open Data Commons Attribution License v1.0**，并提供开放访问。保留的 [原始 LICENSE](bidmc-demo/LICENSE) 还描述了发布者从 MIMIC-II 提取并增加人工标注的处理过程。使用和再分发应遵守来源页许可并保留署名。

数据作者：Marco Pimentel、Alistair Johnson、Peter Charlton、David Clifton。

请引用：Pimentel, M. A. F., et al. *Towards a Robust Estimation of Respiratory Rate from Pulse Oximeters*. IEEE Transactions on Biomedical Engineering, 64(8), 1914–1923. DOI: [10.1109/TBME.2016.2613124](https://doi.org/10.1109/TBME.2016.2613124)。

项目实现的基线直接分析阻抗 RESP 波形；并未复现论文的 PPG 呼吸率估计算法。数值 HR/PULSE/RESP/SpO2 直接来自原始监护仪记录；这些数值不是本项目重新估计的结果。详见 [真实数据评测](../docs/medical-validation.md)。
