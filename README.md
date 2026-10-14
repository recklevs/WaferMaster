# WaferMaster — 晶圆干涉图像分析与异常检测原型

基于 **C++17 / Qt 6 / OpenCV / ONNX Runtime** 的 Windows 桌面项目，支持传统频域纹理分析与 PatchCore 异常检测，以及结果存储、历史查询和 TCP 控制。

> 本项目用于学习与演示，**不是经过光学标定的物理平坦度测量系统**。Good / Warning / NG 是图像指标或模型分数的阈值判定，不代表物理计量验收结论。

## 功能

- **输入**：AVI 视频文件、按文件名排序的图片序列。
- **双模式**：传统模式显示频谱、滤波结果及 FI/P95/HotRatio；PatchCore 模式显示异常分数、热力图及叠加图。
- **观察与显示**：观察 ROI 局部放大、亮度/对比度显示调整、自适应缩放。
- **结果记录**：CSV + SQLite 异步保存，DL 彩色热力图另存 PNG；按等级查询最近最多 1000 条记录。
- **通信与日志**：本机 TCP 控制与状态查询；spdlog 输出到控制台、文件和界面。

采集、算法、存储分别在独立线程工作，主线程负责界面与 TCP 事件处理。

## 截图

<details>
<summary>主界面</summary>

![WaferMaster 主界面](docs/images/overview.png)

</details>

传统模式与 TCP 控制：

![传统频域分析与 TCP 客户端控制](docs/images/traditional-tcp.png)

PatchCore 模式与历史查询：

![PatchCore 异常热力图与历史记录](docs/images/patchcore-history.png)

截图中的分数和 NG 标签仅展示程序运行，不作为准确率或真实缺陷标注。

## 两种模式实际在做什么

### 传统模式：频域纹理分析

处理链：灰度化 → 极坐标展开 → DFT → 圆环带通 → IDFT → 极坐标滤波图中心 ROI 统计。

| 指标 | 实际含义 |
| --- | --- |
| FI | 滤波后 ROI 内像素值的标准差 |
| P95 | 滤波后 ROI 内全部像素值的第 95 百分位 |
| HotRatio | ROI 内滤波值超过固定像素阈值的比例，不是实际缺陷面积 |

默认阈值为 FI ≥ 18、P95 ≥ 30、HotRatio ≥ 5%。三项都超阈值判为 NG，一至两项为 Warning，均未超阈值为 Good。参数见 [Common.h](WaferMaster/include/Common.h)。

这些指标会受到条纹对比度、成像条件和参数影响；滤波结果图还经过逐帧归一化。它们不是表面高度、PV/RMS 或具有物理单位的平坦度。

### PatchCore 模式：学习正常纹理后的异常检测

通过 ONNX Runtime 在 CPU 上推理，读取 `pred_score` 和 `anomaly_map`，显示异常分数、彩色热力图与叠加图。默认 Good / Warning、Warning / NG 分界为 0.76、0.90；异常分数不是缺陷概率。

当前模型预处理为灰度复制到三通道或保留 BGR → resize 至 256×256 → 除以 255 → NCHW，不额外做 ImageNet mean/std 归一化。默认输入为 float32 `[1,3,256,256]`；更换或重新训练模型时，需要同步核对预处理、输入输出和阈值。

当前模型主要基于模拟条纹训练，真实图像存在域差异，弱异常仍可能漏检。**两种模式尚未经过独立真实标注测试集的系统评估，也未验证融合或互补效果。**

## 使用 v2.0 发布包

1. 在 [v2.0 Releases](https://github.com/recklevs/WaferMaster/releases/tag/v2.0) 下载 ZIP，完整解压到可写目录。
2. 保留 exe 同目录的 DLL、插件文件夹及 `models/model_curved_fringe.onnx`，双击 `WaferMaster.exe`。
3. 选择图片文件夹或 AVI 文件、检测模式，再点击“开始检测”；可手动停止，输入自然读完后也会停止。
4. 目标电脑如缺 C++ 运行库，安装随包的 `vc_redist.x64.exe`。

不要只复制 exe，也不要将 Debug 和 Release 的 DLL 混装。

## 从源码构建

### 依赖与准备

- Windows x64，MSVC 2022 / v143、Qt 6.10.2（msvc2022_64）、OpenCV 4.12.0、CMake ≥ 3.25、Ninja。
- spdlog 1.14.1 源码已包含在仓库中；SQLite 通过 Qt SQL 使用。
- **ONNX Runtime 二进制与模型未纳入 Git**，克隆源码后需自行准备：

```text
WaferMaster/onnxruntime/lib/onnxruntime.lib
WaferMaster/onnxruntime/lib/onnxruntime.dll
WaferMaster/models/model_curved_fringe.onnx
```

ONNX Runtime 的头文件、导入库和 DLL 应来自配套 SDK；仓库当前头文件的 API 版本为 27，不要任意混用版本。模型接口和训练背景可参考 [模型交付简报](WaferMaster/models/DL_MODEL_BRIEF.txt)，其中历史接入建议以当前代码为准。

构建配置使用作者本机安装路径。请先按实际环境调整 [CMakeLists.txt](WaferMaster/CMakeLists.txt)、[CMakePresets.json](WaferMaster/CMakePresets.json) 和 [MSVC/CMake 启动脚本](scripts/cmake-vs2022.cmd) 中的 Qt、OpenCV、MSVC、CMake 路径。

### 编译命令

在仓库根目录打开 PowerShell：

```powershell
cd .\WaferMaster

# Debug
..\scripts\cmake-vs2022.cmd --preset vscode
..\scripts\cmake-vs2022.cmd --build --preset vscode-debug

# Release
..\scripts\cmake-vs2022.cmd --preset vscode-release
..\scripts\cmake-vs2022.cmd --build --preset vscode-release
```

输出分别在 `WaferMaster/build-vscode-wrapper-ninja/` 和 `WaferMaster/build-vscode-release/`。

构建后会部署 Qt、OpenCV、ONNX Runtime 和模型。自行整理发布包时，应保留运行 DLL、Qt 插件和模型；AVI 解码还需将 OpenCV 的 `opencv_videoio_ffmpeg4120_64.dll` 放在 exe 旁。不要打包构建缓存或个人历史数据。

## TCP 调试

程序默认监听 **127.0.0.1:9000**，只接受同机客户端。NetAssist 选择 **TCP Client / ASCII**，连接该地址与端口。

协议每行一条命令，必须以实际换行结束。NetAssist 勾选“自动解析转义符”后，可分别输入下表内容；`\n` 中的 `n` 必须小写。

| 发送内容 | 作用 | 响应 |
| --- | --- | --- |
| `START\n` | 请求开始检测 | `OK START` |
| `STOP\n` | 请求停止检测 | `OK STOP` |
| `STATUS\n` | 查询缓存状态和最近结果 | `STATUS state=...` |

START/STOP 的 OK 只表示命令被识别，不保证检测已经启动或停止；实际结果需结合界面、日志与 STATUS 判断。STATUS 在传统模式下返回 FI/P95/HotRatio，在 DL 模式下返回 score/inference_ms，**不是主动广播**。

## 数据与已知限制

- 默认记录位于 exe 目录下的 `logs/records_v2.csv`、`logs/records.db`；DL 彩色展示热力图位于 `logs/heatmaps/`，不是原始浮点异常图。
- 旧版 `records.csv` 不覆盖；SQLite 通过补充字段保留旧记录。CSV/SQLite 双写不是跨文件原子事务，发生故障时可能不一致。
- 采集队列满时丢弃最旧帧，不保证视频逐帧无遗漏；当前不支持工业相机实时采集。
- 观察 ROI 只裁切显示，不改变检测计算。传统滤波图属于极坐标域，不能把左右窗口的同坐标理解为原图中的同一物理位置。
- 历史查询仍会同步等待存储线程返回；模型加载和推理期间，停止/关闭可能需要等待当前操作结束。

本项目用于学习与演示，第三方库、模型与素材的使用和分发需遵守各自许可。
