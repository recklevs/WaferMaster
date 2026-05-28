#pragma once

#include <QObject>
#include <QQueue>
#include <QMutex>
#include <QStringList>
#include <QSize>
#include <opencv2/opencv.hpp>
#include "Common.h"

/// @brief 采集层：从 AVI 视频文件或图片序列文件夹中逐帧读取，
///        将帧克隆后放入线程安全队列，通过信号通知算法线程取帧。
///
/// 线程模型：
///   - 必须在主线程以无父对象方式构造
///   - 构造后 moveToThread() 迁移到独立采集线程
///   - start() / stop() 通过跨线程信号槽触发
///   - tryDequeueFrame() 由算法线程调用（队列锁保护，线程安全）
///
/// 数据所有权：
///   - enqueueFrame() 内部调用 cv::Mat::clone() 深拷贝
///   - 出队后的 FramePacket 所有权转移给调用方
class FrameProducer : public QObject
{
    Q_OBJECT

    // ========================================================================
    // 内部数据结构：帧数据包（图像 + 帧号 + 时间戳 三合一）
    // 用单队列替代三个平行队列，避免丢帧时索引/时间戳错位
    // ========================================================================
    struct FramePacket
    {
        cv::Mat frame;        // 帧图像数据（clone 后的独立副本）
        qint64  frameIdx;     // 帧序号，从 0 开始递增
        qint64  timestampMs;  // 时间戳（毫秒），AVI 取 CAP_PROP_POS_MSEC，
                              //   图片序列取当前系统时间
    };

public:
    /// @brief 构造采集器（必须无父对象，moveToThread 强制要求）
    explicit FrameProducer();
    ~FrameProducer();

    /// @brief 设置输入源配置，必须在 start() 之前调用
    /// @param config 输入源类型、路径、帧间隔、队列容量等
    void setSourceConfig(const SourceConfig& config);

    /// @brief 尝试从队列取出一帧（线程安全，算法线程调用）
    /// @param[out] packet 出队的帧数据包，含图像/帧号/时间戳
    /// @return 队列非空时返回 true，否则 false
    bool tryDequeueFrame(FramePacket& packet);

public slots:
    /// @brief 启动采集循环（跨线程信号槽触发）
    ///
    /// 内部流程：
    ///   1. 根据 m_config.sourceType 走 AVI 或图片序列分支
    ///   2. 打开源后发射 sourceInfoReady(路径, 分辨率)
    ///   3. 循环读取帧 → enqueueFrame() 入队
    ///   4. 退出循环后发射 finished()
    ///   5. 异常发生时发射 errorOccurred(msg)
    void start();

    /// @brief 停止采集循环（跨线程信号槽触发）
    ///        设置 m_running = false，当前帧读取完毕后自然退出
    void stop();

signals:
    /// @brief 帧队列从空变为非空时发射，用于唤醒算法线程取帧
    /// @note 仅在队列 0→1 的瞬间发射一次，避免高帧率下重复空唤醒
    void frameAvailable();

    /// @brief 采集循环正常退出时发射
    void finished();

    /// @brief 采集过程中发生错误时发射
    /// @param message 人类可读的错误描述
    void errorOccurred(const QString& message);

    /// @brief 采集源打开成功后发射，供 UI 显示路径和分辨率
    /// @param path       输入源路径
    /// @param resolution 视频/首帧图像分辨率（宽×高）
    void sourceInfoReady(const QString& path, const QSize& resolution);

private:
    /// @brief 将一帧入队（仅 start() 内部调用）
    ///
    /// 线程安全规则：
    ///   - 入队前对 cv::Mat 执行 clone() 深拷贝
    ///   - 队列满（≥ m_config.maxQueueSize）时丢弃队列头部最旧帧
    ///   - 队列从空变为非空时，发射 frameAvailable() 信号
    ///
    /// @param frame       原始帧（只读，内部 clone 后入队）
    /// @param frameIdx    帧序号
    /// @param timestampMs 时间戳（毫秒）
    /// @return 是否成功入队（队列满导致丢帧时返回 false）
    bool enqueueFrame(const cv::Mat& frame, qint64 frameIdx, qint64 timestampMs);

    // ========================================================================
    // 成员变量
    // ========================================================================
    SourceConfig         m_config;          // 输入源配置（类型/路径/帧间隔/队列容量）
    QQueue<FramePacket>  m_frameQueue;      // 帧数据队列（单队列三合一，线程安全）
    QMutex               m_queueMutex;      // 保护 m_frameQueue 的互斥锁
    bool                 m_running = false; // 运行标志（start() 置 true，stop() 置 false）
    QStringList          m_imageFiles;      // 图片序列模式下的文件路径列表（已排序）
    int                  m_nextImageIndex = 0; // 图片序列模式下的下一张待读取索引
};