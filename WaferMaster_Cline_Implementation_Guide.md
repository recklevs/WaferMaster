# WaferMaster Cline 实施指导文档

## 1. 文档定位

本文档用于约束 `cline` 在 `E:\vsproject\WaferMaster\WaferMaster` 中的后续编码实现。

本文档只保留以下内容：
- 已确认架构蓝图
- 完整接口签名
- 固定数据结构定义
- 线程安全硬规则
- 代码来源与复用边界
- Phase 1 具体验收标准

本文档不再包含以下内容：
- 目标 UI 布局草图
- 拖控件说明
- 对象检查器教程
- UI 迁移步骤教程

已确认前提：
- 代码主工程：`E:\vsproject\WaferMaster\WaferMaster`
- 算法来源：`E:\vsproject\Project2\main.cpp`
- UI 显示层复用来源：`E:\vsproject\QtWidgetsApplication1`
- 当前 `WaferMaster.ui` 骨架已存在
- 通讯层本阶段只预留扩展位，不落具体实现类

---

## 2. 架构蓝图

### 2.1 四层分离图

```text
采集层 -> 算法层 -> 通讯层(预留) -> 表现层
```

类归属固定为：
- 采集层：`FrameProducer`
- 算法层：`WaferAlgorithm`
- 通讯层：预留给后续 TCP / 外部触发模块，本阶段不实现
- 表现层：`MainWindow`

必须明确：
- 当前实际运行主链路是 `FrameProducer -> WaferAlgorithm -> MainWindow`
- 通讯层不参与 Phase 1 主链路，但必须在架构中保留独立层位
- `MainWindow` 只负责显示、交互、线程编排，不负责采集和算法

### 2.2 线程归属表

| 所属线程 | 类 / 职责 |
| --- | --- |
| 主线程 | `MainWindow`、所有 `QWidget / QLabel / QStatusBar`、观察 ROI 交互、`cv::Mat -> QImage` 转换与显示刷新 |
| 采集线程 | `FrameProducer` |
| 算法线程 | `WaferAlgorithm` |
| 通讯线程 | Phase 1 不创建；后续若接 TCP，默认独立线程，禁止并入主线程或算法线程 |

线程规则固定为：
- 禁止继承 `QThread`
- 只允许 `QObject + moveToThread()`
- `FrameProducer`、`WaferAlgorithm` 必须以无父对象方式创建后再迁移线程

### 2.3 数据流完整链路图

```text
[磁盘: AVI / 图片序列]
        ↓ 读取一帧
[FrameProducer 线程]
        ↓ frame.clone()
        ↓ QQueue<cv::Mat> + QMutex
        ↓ 队列满时丢最旧帧
        ↓ 队列从空变非空时 emit frameAvailable()
[WaferAlgorithm 线程]
        ↓ 取出一帧
        ↓ 灰度 → warpPolar → DFT → 频谱中心化
        ↓ 圆环带通掩膜 → IDFT
        ↓ 算法固定ROI统计
        ↓ FI + p95 + HotRatio
        ↓ classify() -> Good/Warning/NG
        ↓ 生成 original / spectrum / flatness
        ↓ emit resultReady(AlgoResult)
[MainWindow 主线程]
        ↓ cv::Mat -> QImage
        ↓ 刷新 原图 / 频谱图 / 平坦图
        ↓ 更新 statusBar
        ↓ 如用户已选观察ROI，再裁出 ROI原图 / ROI结果图
[屏幕显示]
```

必须明确：
- 观察 ROI 不参与算法统计
- `AlgoResult` 是算法层到表现层的唯一结果载体
- 通讯层仅预留接口，Phase 1 不落具体类

### 2.4 三条线程安全硬规则

以下规则禁止更改口径：

1. 跨线程 `cv::Mat` 必须 `clone()`
   - 入队前深拷贝
   - 算法结果持有独立所有权
   - UI 线程只读

2. `QQueue<cv::Mat>` 的所有读写必须由同一把 `QMutex` 保护
   - 禁止裸访问

3. 所有 UI 控件只能在主线程操作
   - 采集线程和算法线程禁止触碰 `QWidget`、`QLabel`、`QStatusBar`

补充硬规则：
- `AlgoResult` 必须 `Q_DECLARE_METATYPE`
- 应用启动时必须执行 `qRegisterMetaType<AlgoResult>()`

---

## 3. 已确认技术决策清单

### 3.1 传帧与唤醒

- 传帧存储：`QQueue<cv::Mat> + QMutex`
- 传帧通知：Qt 信号槽
- 原因：队列负责解耦生产消费速率，信号槽只负责唤醒

### 3.2 队列策略

- 队列上限：`maxSize = 10`
- 队列满时丢最旧帧，保留最新帧
- 原因：本项目优先实时响应，不追求逐帧零丢失

### 3.3 `frameAvailable()` 触发时机

- 只有队列从空变非空时才发一次
- 原因：减少高帧率下重复空唤醒和线程切换

### 3.4 ROI 双线分离

- 算法 ROI
  - 算法内部固定 ROI
  - 参与 `fi / p95 / hotRatio`

- 观察 ROI
  - 界面手动框选
  - 只用于 `ROI原图 / ROI结果图`

- 原因：统计链与显示链分离，互不污染

### 3.5 `AlgoResult.level` 类型

- 正式类型：`enum class DetectionLevel`
- UI 和 CSV 通过统一转换函数输出 `Good / Warning / NG`

### 3.6 线程模型

- 禁止继承 `QThread`
- 只允许 `moveToThread()`

### 3.7 跨线程 `cv::Mat` 规则

- 入队前 `clone()`
- 算法输出到 `AlgoResult` 时确保独立所有权
- UI 线程只读显示

### 3.8 `FrameProducer` 构造约束

- 构造接口不带父对象参数
- 实例化时必须无 `parent`

---

## 4. 固定接口与类型定义

### 4.1 `Common.h`

以下接口签名与字段定义作为正式口径，供实现者直接照写：

```cpp
#pragma once

#include <QString>
#include <QMetaType>
#include <QRect>
#include <opencv2/opencv.hpp>

enum class InputSourceType
{
    AviVideo,
    ImageSequence
};

enum class RunState
{
    Idle,
    Running,
    Stopped,
    Error
};

enum class DetectionLevel
{
    Good,
    Warning,
    Ng
};

struct SourceConfig
{
    InputSourceType sourceType = InputSourceType::ImageSequence;
    QString sourcePath;
    int frameIntervalMs = 33;
    int maxQueueSize = 10;
};

struct AlgoConfig
{
    double fiThreshold = 18.0;
    double p95Threshold = 30.0;
    double hotRatioThreshold = 5.0;
    double roiRatioW = 0.70;
    double roiRatioH = 0.70;
    double bandPassInnerRatio = 0.02;
    double bandPassOuterRatio = 0.15;
};

struct AlgoResult
{
    qint64 frameIdx = -1;
    qint64 timestampMs = 0;
    double fi = 0.0;
    double p95 = 0.0;
    double hotRatio = 0.0;
    DetectionLevel level = DetectionLevel::Good;
    QRect algoRoiRect;
    cv::Mat frameOriginal;
    cv::Mat frameSpectrum;
    cv::Mat frameFlatness;
};

QString detectionLevelToString(DetectionLevel level);
Q_DECLARE_METATYPE(AlgoResult)
```

字段口径说明：
- `AlgoConfig` 以数据流确认版为准，正式包含 `fi / p95 / hotRatio`
- `p95Threshold` 用于 Hot 区判定阈值口径，替代 `Project2` 中 `hotThreshAbs` 的旧表达
- `bandPassInnerRatio / bandPassOuterRatio` 来自 `Project2` 中圆环带通掩膜的比例语义
- `algoRoiRect` 是算法 ROI，不是观察 ROI

### 4.2 `FrameProducer`

以下接口签名作为正式口径：

```cpp
#pragma once

#include <QObject>
#include <QQueue>
#include <QMutex>
#include <QStringList>
#include <QSize>
#include <opencv2/opencv.hpp>
#include "Common.h"

class FrameProducer : public QObject
{
    Q_OBJECT

public:
    explicit FrameProducer();
    ~FrameProducer();

    void setSourceConfig(const SourceConfig& config);
    bool enqueueFrame(const cv::Mat& frame, qint64 frameIdx, qint64 timestampMs);
    bool tryDequeueFrame(cv::Mat& frame, qint64& frameIdx, qint64& timestampMs);

public slots:
    void start();
    void stop();

signals:
    void frameAvailable();
    void finished();
    void errorOccurred(const QString& message);
    void sourceInfoReady(const QString& path, const QSize& resolution);

private:
    SourceConfig m_config;
    QQueue<cv::Mat> m_frameQueue;
    QQueue<qint64> m_frameIndices;
    QQueue<qint64> m_timestampsMs;
    QMutex m_queueMutex;
    bool m_running = false;
    QStringList m_imageFiles;
    int m_nextImageIndex = 0;
};
```

接口语义固定为：
- `start()` 根据 `sourceType` 自动走 AVI 或图片序列分支
- `enqueueFrame(...)` 内部必须 `clone()` 后入队
- 队列满时丢最旧帧及其配套索引与时间戳
- 只有队列从空变非空时才发 `frameAvailable()`
- `FrameProducer` 不做任何算法和 UI 逻辑

### 4.3 `WaferAlgorithm`

以下接口签名作为正式口径：

```cpp
#pragma once

#include <QObject>
#include <QString>
#include "Common.h"

class FrameProducer;

class WaferAlgorithm : public QObject
{
    Q_OBJECT

public:
    explicit WaferAlgorithm();
    ~WaferAlgorithm();

    void setAlgoConfig(const AlgoConfig& config);
    void setFrameProducer(FrameProducer* producer);

public slots:
    void start();
    void processPendingFrames();
    void stop();

signals:
    void resultReady(const AlgoResult& result);
    void finished();
    void errorOccurred(const QString& message);

private:
    AlgoResult processSingleFrame(const cv::Mat& frame, qint64 frameIdx, qint64 timestampMs) const;
    void shiftDFT(cv::Mat& fImage) const;
    QRect makeCentralRoi(const QSize& size) const;
    float percentile95(const cv::Mat& roi32f) const;
    DetectionLevel classify(double fi, double p95, double hotRatio) const;

    AlgoConfig m_config;
    FrameProducer* m_producer = nullptr;
    bool m_running = true;
};
```

接口语义固定为：
- `start()` 在 MainWindow 点击"开始检测"时调用，重置 `m_running = true`，允许 `processPendingFrames` 再次被唤醒
- `stop()` 调用后 `m_running = false`，并 `emit finished()`
- `processPendingFrames()` 由 `frameAvailable()` 触发
- 被唤醒后循环从 `FrameProducer` 拉帧，直到队列清空
- `processSingleFrame(...)` 内部完整承接 `Project2` 算法主链
- `resultReady(...)` 发出前，`AlgoResult` 中的三张 `cv::Mat` 必须可安全跨线程显示

### 4.4 `MainWindow`

以下接口签名作为正式口径：

```cpp
#pragma once

#include <QMainWindow>
#include <QThread>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QPoint>
#include <opencv2/opencv.hpp>
#include "Common.h"

class QLabel;
class FrameProducer;
class WaferAlgorithm;

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onBrowseClicked();
    void onStartClicked();
    void onStopClicked();
    void onObserveRoiToggled(bool checked);
    void onSourceInfoReady(const QString& path, const QSize& resolution);
    void onAlgorithmResultReady(const AlgoResult& result);
    void onWorkerError(const QString& message);
    void onProducerFinished();
    void onAlgorithmFinished();

private:
    void setupUiState();
    void setupWorkers();
    void setupConnections();
    void cleanupWorkers();

    SourceConfig buildSourceConfig() const;
    void updateRunState(RunState state);
    void updateStatusBarText();
    void refreshMainViews(const AlgoResult& result);
    void refreshObserveRoiViews();

    QImage cvMatToQImage(const cv::Mat& mat) const;
    void showMatOnLabel(QLabel* label, const cv::Mat& mat) const;

    QRect mapLabelRectToImageRect(QLabel* label, const QSize& imageSize, const QRect& labelRect) const;
    QRect normalizedObserveRect() const;

private:
    Ui::MainWindow* ui = nullptr;

    QThread* m_producerThread = nullptr;
    QThread* m_algorithmThread = nullptr;
    FrameProducer* m_producer = nullptr;
    WaferAlgorithm* m_algorithm = nullptr;

    RunState m_runState = RunState::Idle;
    SourceConfig m_sourceConfig;
    AlgoResult m_lastResult;

    QString m_currentPath;
    QSize m_currentResolution;

    bool m_observeRoiEnabled = false;
    bool m_isSelectingObserveRoi = false;
    QPoint m_observeStartPoint;
    QPoint m_observeEndPoint;
    QRect m_observeRoiRect;
};
```

接口语义固定为：
- `MainWindow` 只做线程编排、显示刷新、状态管理、观察 ROI 交互
- `cvMatToQImage(...)` 直接参考 `QtWidgetsApplication1`
- `showMatOnLabel(...)` 封装 `cvMatToQImage` + `QLabel::setPixmap`
- 观察 ROI 只允许在 `Idle / Stopped` 状态使用
- `refreshObserveRoiViews()` 从 `m_lastResult.frameOriginal / frameFlatness` 裁出两张 ROI 小图
- `onSourceInfoReady(...)` 接 `FrameProducer::sourceInfoReady` 信号，更新路径和分辨率
- `buildSourceConfig()` 从 UI 控件收集中断值构造 `SourceConfig`
- `eventFilter()` 实现原图 QLabel 上的鼠标框选交互
- `mapLabelRectToImageRect()` 将 QLabel 像素坐标映射到实际图像像素坐标
- `MainWindow` 不做算法计算，不读帧，不写 CSV

MainWindow 实施分两阶段：
1. Phase 1 主链路：线程创建、开始/停止、`qRegisterMetaType<AlgoResult>()`、三图刷新、状态栏基础信息
2. Phase 2 观察 ROI：`eventFilter()`、原图框选、ROI 原图、ROI 结果图、运行中禁用观察 ROI

---

## 5. 表现层逻辑范围

本文档不再重新定义 UI 布局，只确认当前 UI 骨架下的控件职责：

- 输入源相关控件
  - 负责选择 `AVI / 图片序列`
  - 负责路径选择与显示

- 运行控制控件
  - 负责开始检测
  - 负责停止检测
  - 负责观察 ROI 开关

- 三张主图
  - 原图：显示输入帧
  - 频谱图：显示频谱结果
  - 平坦图：显示最终检测结果

- 两张 ROI 小图
  - `ROI原图`
  - `ROI结果图`

- 状态栏
  - 路径或文件信息
  - 分辨率
  - 当前帧号
  - `fi`
  - `p95`
  - `hotRatio`
  - `level`
  - 当前运行状态

表现层约束：
- 后续实现必须贴合当前已完成的 `WaferMaster.ui`
- 不再要求 `cline` 重做目标 UI 布局

---

## 6. 代码来源与复用边界

### 6.1 算法来源：`E:\vsproject\Project2\main.cpp`

直接承接的算法语义：
- `shiftDFT`
- 中心 ROI 生成思路
- `warpPolar`
- DFT / 频谱中心化
- 圆环带通掩膜
- IDFT
- 平坦图生成
- `flatness_metrics.csv` 输出语义

但必须改造成新接口口径：
- 指标字段以 `fi / p95 / hotRatio` 为准
- 分类函数内部返回 `DetectionLevel`
- 不再使用 `cv::imshow(...)`

### 6.2 UI 复用来源：`E:\vsproject\QtWidgetsApplication1`

允许复用的代码思路：
- `cvMatToQImage(...)`
- 状态栏基础图像信息显示
- 亮度 / 对比度仅影响显示
- 原图上的鼠标框选交互经验

明确禁止原样搬运：
- `cv::imshow("ROI Preview", ...)`
- 旧版 `extractROI()` 的简化坐标换算直接照搬
- 把 ROI 交互、图像读写、算法处理全部塞进主窗口类

---

## 7. Phase 1 具体验收标准

### 7.1 构建与启动

- `WaferMaster` 在 VS2022 / CMake 下可成功 configure、build、run
- 启动后主窗口正常显示
- 主流程不依赖任何 `cv::imshow(...)` 弹窗

### 7.2 输入源

- 能在界面中选择 `AVI` 或图片序列
- 选择路径后点击开始，`FrameProducer` 能持续产帧
- 错误路径通过错误信号回到主线程显示
- 错误路径不会导致程序直接崩溃

### 7.3 主链路

- 点击"开始检测"后，原图、频谱图、平坦图连续刷新
- 点击"停止检测"后，线程安全停机，界面保留最后一帧结果
- 停止后再次开始，不需要重启程序即可继续工作

### 7.4 指标与状态

状态栏必须能显示：
- 路径或文件信息
- 分辨率
- 当前帧号
- `fi`
- `p95`
- `hotRatio`
- `level`
- 当前运行状态

补充约束：
- `level` 文本必须来自 `DetectionLevel -> QString` 的统一转换

### 7.5 观察 ROI

- 观察 ROI 只能在 `Idle / Stopped` 状态框选
- 框选后必须同步得到：
  - 原图上的观察框
  - 平坦图上的同一观察框
  - `ROI原图`
  - `ROI结果图`
- 观察 ROI 的改变不影响下一帧算法结果

### 7.6 线程与稳定性

- UI 线程无直接算法计算
- UI 线程无直接读帧
- 采集线程和算法线程均通过 `moveToThread()` 运行
- 队列满时会丢最旧帧，不会无限堆积
- 连续运行 5 分钟内无明显卡死、未响应、跨线程 UI 操作错误
- 运行期间不会因 `cv::Mat` 共享导致随机崩溃或脏图

---

## 8. 默认假设

- `WaferMaster.ui` 已作为既定表现层载体，不再在本文档中定义目标布局
- 通讯层只预留架构位置，Phase 1 不落具体类
- 所有后续实现必须兼容 `Qt6 + CMake + OpenCV + MSVC + VS2022`
- 所有源文件与文档保持 UTF-8 与标准 Windows CRLF