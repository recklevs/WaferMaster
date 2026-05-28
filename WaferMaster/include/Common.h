#pragma once

#include <QString>
#include <QMetaType>
#include <QRect>
#include <opencv2/opencv.hpp>

// ============================================================================
// 枚举定义
// ============================================================================

/// @brief 输入源类型：AVI视频文件 或 图片序列文件夹
enum class InputSourceType
{
    AviVideo,       // AVI 视频文件
    ImageSequence   // 图片序列（按文件名排序连续读取）
};

/// @brief 系统运行状态，用于控制 UI 按钮可用性和线程启停
enum class RunState
{
    Idle,    // 空闲，未开始检测
    Running, // 运行中，正在采集+检测
    Stopped, // 已停止，保留最后一帧结果
    Error    // 错误状态（如文件无法打开）
};

/// @brief 检测等级：Good / Warning / NG，由 classify() 根据阈值判定
enum class DetectionLevel
{
    Good,    // 良品，指标均在阈值内
    Warning, // 警告，部分指标接近阈值
    Ng       // 不合格，指标超出阈值
};

// ============================================================================
// 配置结构体
// ============================================================================

/// @brief 输入源配置，FrameProducer 启动时使用
struct SourceConfig
{
    InputSourceType sourceType = InputSourceType::ImageSequence; // 输入源类型，默认图片序列
    QString sourcePath;                                          // 输入源路径（AVI文件路径 或 图片文件夹路径）
    int frameIntervalMs = 33;                                    // 帧间隔（ms），约30fps；图片序列模拟帧率用
    int maxQueueSize = 10;                                       // 帧队列最大长度，满时丢最旧帧
};

/// @brief 算法参数配置，WaferAlgorithm 初始化时传入
struct AlgoConfig
{
    double fiThreshold       = 18.0;  // FI 平坦度阈值，超出则判定异常
    double p95Threshold      = 30.0;  // P95 阈值，Hot 区判定用（替代 Project2 中 hotThreshAbs）
    double hotRatioThreshold = 5.0;   // HotRatio 阈值（Hot区像素占比），超出则判定异常
    double roiRatioW         = 0.70;  // 算法 ROI 宽度占原图的比例（0~1），中心裁切
    double roiRatioH         = 0.70;  // 算法 ROI 高度占原图的比例（0~1），中心裁切
    double bandPassInnerRatio = 0.02; // 带通滤波器内径比例（相对频谱最大半径），低频截止
    double bandPassOuterRatio = 0.15; // 带通滤波器外径比例（相对频谱最大半径），高频截止
};

// ============================================================================
// 结果载体结构体
// ============================================================================

/// @brief 算法层输出结果，是 WaferAlgorithm → MainWindow 的唯一数据载体
///        跨线程传递前必须在 main() 中 qRegisterMetaType<AlgoResult>()
struct AlgoResult
{
    qint64 frameIdx    = -1;          // 帧序号，从0开始递增
    qint64 timestampMs = 0;           // 时间戳（ms），采集时记录
    double fi          = 0.0;         // FI 平坦度指标
    double p95         = 0.0;         // P95 指标（热区像素强度第95百分位值）
    double hotRatio    = 0.0;         // HotRatio：热区像素占总像素的比例（0~1）
    DetectionLevel level = DetectionLevel::Good; // 检测等级，由 classify() 根据阈值判定
    QRect algoRoiRect;                 // 算法 ROI 矩形区域（注意：不是观察ROI）
    cv::Mat frameOriginal;             // 原始输入帧（clone 后的独立副本）
    cv::Mat frameSpectrum;             // 频谱图（处理中间结果，用于显示）
    cv::Mat frameFlatness;             // 平坦图（最终检测结果图，用于显示）
};

// ============================================================================
// 工具函数
// ============================================================================

/// @brief 将检测等级枚举转为可读的中文字符串，供状态栏和CSV使用
QString detectionLevelToString(DetectionLevel level);

// ============================================================================
// Qt 元类型注册声明（跨线程信号槽传参必需）
// ============================================================================
Q_DECLARE_METATYPE(AlgoResult)