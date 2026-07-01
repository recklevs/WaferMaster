#pragma once

#include <QString>
#include <QMetaType>
#include <QRect>
#include <QColor>
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

/// @brief 结果存储模式：无 / CSV / SQLite / 双写
enum class StorageMode
{
    None,   // 不存储
    Csv,    // 仅 CSV 文件
    Sqlite, // 仅 SQLite 数据库
    Both    // CSV + SQLite 双写
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

/// @brief 帧数据包，采集层入队时打包，算法层出队时解包
struct FramePacket
{
    cv::Mat frame;        ///< 帧图像（clone 后的独立副本）
    qint64  frameIdx;     ///< 帧序号，从0开始递增
    qint64  timestampMs;  ///< 时间戳（ms）
};

/// @brief 算法参数配置，WaferAlgorithm 初始化时传入
struct AlgoConfig
{
    double fiThreshold        = 18.0;  // FI 平坦度判定阈值
    double p95Threshold       = 30.0;  // P95 热区像素强度阈值（Hot区像素亮度上限）
    double hotRatioThreshold  = 5.0;   // HotRatio 判定阈值（Hot区像素占比，%），超出则判定异常
    double roiRatioW          = 0.70;  // 算法 ROI 宽度占原图的比例（0~1），中心裁切
    double roiRatioH          = 0.70;  // 算法 ROI 高度占原图的比例（0~1），中心裁切
    double bandPassInnerRatio = 0.02;  // 带通滤波器内径比例（相对频谱最大半径），低频截止
    double bandPassOuterRatio = 0.15;  // 带通滤波器外径比例（相对频谱最大半径），高频截止
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
    double hotRatio    = 0.0;         // HotRatio：热区像素占总像素的比例（0~100%）
    DetectionLevel level = DetectionLevel::Good; // 检测等级，由 classify() 根据阈值判定
    QRect algoRoiRect;                 // 算法 ROI 矩形区域
    cv::Mat frameOriginal;             // 原始输入帧（clone 后的独立副本）
    cv::Mat frameSpectrum;             // 频谱图（处理中间结果，用于显示）
    cv::Mat frameFlatness;             // 平坦图（最终检测结果图，用于显示）
};

// ============================================================================
// 工具函数
// ============================================================================

/// 将检测等级枚举转为可读字符串，供状态栏使用
inline QString detectionLevelToString(DetectionLevel level)
{
    switch (level)
    {
    case DetectionLevel::Good:    return QStringLiteral("Good");
    case DetectionLevel::Warning: return QStringLiteral("Warning");
    case DetectionLevel::Ng:      return QStringLiteral("NG");
    default:                      return QStringLiteral("Unknown");
    }
}

/// 根据检测等级返回对应颜色，供状态栏和 ROI 矩形框显示使用
/// Good=绿色, Warning=橙色, Ng=红色, Unknown=灰色
inline QColor levelToColor(DetectionLevel level)
{
    switch (level)
    {
    case DetectionLevel::Good:    return QColor(0, 180, 0);       // 绿色
    case DetectionLevel::Warning: return QColor(255, 180, 0);     // 橙色
    case DetectionLevel::Ng:      return QColor(220, 30, 30);     // 红色
    default:                      return QColor(128, 128, 128);   // 灰色
    }
}

// ============================================================================
// Qt 元类型注册声明（跨线程信号槽传参必需）
// ============================================================================
Q_DECLARE_METATYPE(AlgoResult)

#include "Logger.h"
