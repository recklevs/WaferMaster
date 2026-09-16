#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include "Common.h"
#include "DeepWaferDetector.h"

class FrameProducer;

/**
 * @brief 算法层：对每一帧执行平坦度检测。
 *
 * 传统模式：
 *   灰度 → warpPolar → DFT → 带通掩膜 → IDFT
 *   → FI / P95 / HotRatio → 等级判断
 *
 * 深度学习模式：
 *   调用 DeepWaferDetector 执行 ONNX 推理。
 *
 * 线程说明：
 *   1. WaferAlgorithm 由主线程创建；
 *   2. 启动前加载配置和模型；
 *   3. 随后移动到算法线程；
 *   4. start() 和 processPendingFrames() 在算法线程执行；
 *   5. requestStop() 只修改原子停止标志，可由主线程直接调用；
 *   6. requestStop() 不会中断当前正在执行的 Run()。
 *
 * 工作流程：
 *   1. MainWindow 设置算法配置、采集器指针和检测模式；
 *   2. 深度学习模式下加载 ONNX 模型；
 *   3. FrameProducer 产生新帧并发出 frameAvailable()；
 *   4. processPendingFrames() 被唤醒并循环取帧；
 *   5. 根据检测模式选择传统算法或深度学习算法；
 *   6. 每处理完成一帧，发出 resultReady()；
 *   7. 队列为空时返回，等待下一次 frameAvailable()；
 *   8. m_running.load() 为 false 时停止处理；
 *   9. 当前帧完成后，算法线程自然退出；
 *  10. 主线程 wait() 确认线程退出后，再释放算法对象。
 *
 * stop() 槽除了设置停止标志外，还会发出 finished()；
 * requestStop() 只设置停止标志，不发出 finished()。
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

    /// @brief 设置检测模式（Traditional / DeepLearning）
    void setDetectionMode(DetectionMode mode);
    /// @brief 加载 DL 模型
    bool loadDLModel(const DLconfig& config);
    /// @brief 模型是否已加载
    bool isDLModelLoaded() const;

    /// @brief 立即请求停止，可从任意线程调用。
    ///        只修改原子标志，不等待事件队列，也不会中断当前正在执行的 Run()。
    void requestStop() noexcept
    {
        m_running.store(false);
    }

public slots:
    /// @brief 启动算法处理（重置运行标志，允许 processPendingFrames 再次被唤醒）
    void start();

    /**
     * @brief 消费者：处理队列中的待处理帧。
     *
     * 处理流程：
     *   1. 使用 tryDequeueFrame() 非阻塞取出一帧；
     *   2. 深度学习模式调用 m_deepdetector.processFrame()；
     *   3. 传统模式调用 processSingleFrame()；
     *   4. 发出 resultReady()，将结果交给主窗口；
     *   5. 队列为空时返回；
     *   6. 每次处理下一帧前检查 m_running.load()；
     *   7. 停止请求到达后，当前帧完成，循环自然退出。
     */
    void processPendingFrames();

    /// @brief 停止算法处理循环（Qt 槽）
    ///        原子设置 m_running=false，当前帧处理完毕后自然退出
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

    /**  @brief 对单帧执行完整OpenCV处理
    传统算法步骤：
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
    std::atomic_bool  m_running{false};      // 运行标志（构造初始为 false),原子变量保证一次读或写操作不会被打断，并且其他线程能及时看到最新值
    DetectionMode   m_detectionMode = DetectionMode::Traditional; // 检测模式（Traditional / DeepLearning）
    DeepWaferDetector m_deepdetector;        // 深度学习检测器
};
