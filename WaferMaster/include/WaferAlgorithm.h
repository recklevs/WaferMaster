#pragma once

#include <QObject>
#include <QString>
#include "Common.h"

class FrameProducer;

/**  @brief 算法层：平坦度检测主链，对每一帧执行
          灰度→warpPolar→DFT→带通掩膜→IDFT→FI/p95/HotRatio→classify

## WaferAlgorithm 工作流程

1. setAlgoConfig() — 设好阈值参数（fi/p95/hotRatio/ROI比例/带通比例）

2. setFrameProducer() — 注入采集层指针，用于拉帧

3. frameAvailable() 信号触发 → processPendingFrames() 槽被调用
     │
     ├─ 循环调用 m_producer->tryDequeueFrame() 拉帧
     │
4. processSingleFrame() — 执行完整算法主链：
     │  灰度 → warpPolar → DFT → shiftDFT 频谱中心化
     │  → 圆环带通掩膜 → IDFT → makeCentralRoi 生成算法ROI
     │  → percentile95 统计 → FI + p95 + HotRatio
     │
5. classify() — 根据三指标阈值判定 Good/Warning/NG
     │
 6. resultReady() — 发出 AlgoResult（含 original/spectrum/flatness 三图）
      │
 7. stop() → m_running=false → processPendingFrames() 循环退出
      │
 8. stop() 中发射 finished()
 */
class WaferAlgorithm : public QObject
{
    Q_OBJECT

public:
    /// @brief 构造算法处理器（必须无父对象，moveToThread 强制要求）
    explicit WaferAlgorithm();
    ~WaferAlgorithm();

    /// @brief 设置算法参数配置
    /// @param config 包含 fi/p95/hotRatio 阈值、ROI 比例、带通比例等
    void setAlgoConfig(const AlgoConfig& config);

    /// @brief 注入采集层指针，供 processPendingFrames() 拉帧使用
    /// @param producer FrameProducer 实例指针（由 MainWindow 创建并迁移线程后传入）
    void setFrameProducer(FrameProducer* producer);

public slots:
    /// @brief 启动算法处理（重置运行标志，允许 processPendingFrames 再次被唤醒）
    void start();

    /**  @brief 消费者：处理队列中所有待处理帧（由 frameAvailable() 信号触发）
    内部流程：
        1. 循环调用 m_producer->tryDequeueFrame() 拉帧
        2. 每取到一帧 → processSingleFrame()
        3. emit resultReady(result)
        4. 队列清空后直接返回，等待下一次 frameAvailable() 唤醒
        5. m_running==false 时立即退出循环*/
    void processPendingFrames();

    /// @brief 停止算法处理循环
    ///        设置 m_running = false，当前帧处理完毕后自然退出
    void stop();

signals:
    /// @brief 单帧算法处理完成时发射，携带完整检测结果
    /// @param result 包含 fi/p95/hotRatio/level 和三张显示用 cv::Mat
    /// @note AlgoResult 中的 cv::Mat 已确保独立所有权，可安全跨线程显示
    void resultReady(const AlgoResult& result);

    /// @brief 算法处理循环正常退出时发射
    void finished();

    /// @brief 算法处理过程中发生错误时发射
    /// @param message 可读的错误描述
    void errorOccurred(const QString& message);

private:
    // ========================================================================
    // 算法主链入口
    // ========================================================================

    /**  @brief 对单帧执行完整算法主链（承接 Project2 算法语义）
    算法步骤：
       1. 灰度转换
       2. warpPolar 极坐标变换
       3. DFT 傅里叶变换 → shiftDFT 频谱中心化
       4. 圆环带通掩膜（由 bandPassInnerRatio/bandPassOuterRatio 控制）
       5. IDFT 逆变换
       6. makeCentralRoi() 生成算法 ROI
       7. percentile95() 统计 → FI / p95 / hotRatio
    @param frame       输入帧（采集层 clone 后的独立副本）
    @param frameIdx    帧序号
    @param timestampMs 时间戳（毫秒）
    @return 完整的 AlgoResult，含三张显示图和检测指标*/
    AlgoResult processSingleFrame(const cv::Mat& frame, qint64 frameIdx, qint64 timestampMs) const;

    // ========================================================================
    // 工具函数
    // ========================================================================

    /**  @brief DFT 频谱象限对角交换（低频移到中心）
    将四个象限两两交换：
       A B      D C
       C D  →   B A
    @param[in,out] fImage DFT 频谱图像（原地修改）*/
    void shiftDFT(cv::Mat& fImage) const;

    /// @brief 生成以图像中心为基准的算法 ROI 矩形
    /// @param size 原始图像尺寸（宽×高）
    /// @return 中心裁切后的 ROI 矩形（由 roiRatioW / roiRatioH 控制比例）
    QRect makeCentralRoi(const QSize& size) const;

    /// @brief 对算法 ROI 浮点图像统计第 95 百分位像素强度
    /// @param roi32f 输入 ROI 图像（CV_32F 单通道）
    /// @return 第 95 百分位亮度值
    float percentile95(const cv::Mat& roi32f) const;

    /// @brief 根据三指标阈值综合判定检测等级
    /// @param fi       FI 平坦度值
    /// @param p95      P95 热区像素强度值
    /// @param hotRatio HotRatio 热区像素占比（%）
    /// @return Good / Warning / NG
    DetectionLevel classify(double fi, double p95, double hotRatio) const;

    // ========================================================================
    // 成员变量
    // ========================================================================
    AlgoConfig      m_config;              // 算法参数配置（阈值/ROI比例/带通比例）
    FrameProducer*  m_producer = nullptr;  // 采集层指针（用于拉帧，不持有所有权）
    bool            m_running = true;      // 运行标志（构造初始为 true，stop() 置 false）
};