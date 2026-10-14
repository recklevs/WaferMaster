#pragma once

#include <QString>
#include <QMetaType>
#include <QRect>
#include <QSize>
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

/// @brief 图像分析等级：Good / Warning / NG，按各模式阈值判定，不代表已标定的物理合格等级
enum class DetectionLevel
{
    Good,    // 当前规则下未触发异常
    Warning, // 当前规则下需要关注
    Ng       // 当前规则下判定异常
};

/// @brief 结果存储模式：无 / CSV / SQLite / 双写
enum class StorageMode
{
    None,   // 不存储
    Csv,    // 仅 CSV 文件
    Sqlite, // 仅 SQLite 数据库
    Both    // CSV + SQLite 双写
};

/// @brief 检测模式：传统 OpenCV 算法 或 PatchCore 深度学习
enum class DetectionMode
{
    Traditional,   // OpenCV DFT 频域分析
    DeepLearning   // PatchCore ONNX 推理
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
    double fiThreshold        = 18.0;  // 滤波后 ROI 灰度标准差（FI）的经验阈值
    double p95Threshold       = 30.0;  // P95 判定阈值，同时用作 HotRatio 的像素筛选阈值
    double hotRatioThreshold  = 5.0;   // HotRatio 经验阈值（%）；最终等级由三项共同判定
    double roiRatioW          = 0.70;  // ROI 宽度占极坐标滤波图的比例（0~1），中心裁切
    double roiRatioH          = 0.70;  // ROI 高度占极坐标滤波图的比例（0~1），中心裁切
    double bandPassInnerRatio = 0.02;  // 内半径占频谱短边长度的比例，低频截止
    double bandPassOuterRatio = 0.15;  // 外半径占频谱短边长度的比例，高频截止
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
    double fi          = 0.0;         // FI：滤波后 ROI 的灰度标准差
    double p95         = 0.0;         // 滤波后 ROI 内全部像素值的第 95 百分位
    double hotRatio    = 0.0;         // ROI 内滤波值超过 p95Threshold 的比例（0~100%）
    DetectionLevel level = DetectionLevel::Good; // 检测等级，由 classify() 根据阈值判定
    QRect algoRoiRect;                 // 传统模式为极坐标滤波图 ROI；DL 模式未使用
    cv::Mat frameOriginal;             // 原始输入帧（clone 后的独立副本）
    cv::Mat frameSpectrum;             // 频谱图（处理中间结果，用于显示）
    cv::Mat frameFlatness;             // 历史字段名保留：传统滤波图 / DL 叠加图，非高度图

    // —— DL 模式扩展字段（Traditional 模式下为默认值）——
    DetectionMode mode          = DetectionMode::Traditional;
    double        anomalyScore  = 0.0; // 模型异常分数
    qint64        inferenceTimeMs = 0;
    cv::Mat       frameHeatmap;       // DL 彩色展示热力图

    // 存储/历史查询信息：源帧时间与保存时间分开，避免混淆视频播放位置和日期时间。
    qint64 recordedAtMs = 0;
    QString heatmapPath;              // 保存的 PNG 文件路径
    QSize heatmapSize;
    bool hasModeInfo = true;          // 旧记录没有检测模式，查询时设为 false
};

///@brief 深度学习模型配置
struct DLconfig
{
    QString modelPath;                 // ONNX 模型文件路径
    int     inputSize       = 256;     // 模型输入尺寸
    float   goodThreshold   = 0.76f; // Good/NG 判定阈值
    float   ngThreshold     = 0.90f;
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

/// 检测模式文本，用于结果存储和历史查询。
inline QString detectionModeToString(DetectionMode mode)
{
    return mode == DetectionMode::DeepLearning
        ? QStringLiteral("DeepLearning") : QStringLiteral("Traditional");
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
