#pragma once

#include <QObject>
#include <QQueue>
#include <QMutex>
#include <QTimer>
#include <QStringList>
#include <QSize>
#include <opencv2/opencv.hpp>
#include "Common.h"

/**  @brief 采集层：从 AVI 视频文件或图片序列文件夹中逐帧读取，
            QTimer 驱动，stop() 在采集线程执行,可中断。

## FrameProducer 工作流程 (QTimer 版)

1. setSourceConfig() — 设好"去哪读"（路径/类型/帧间隔）

2. start() — 打开源 → 发射 sourceInfoReady → 创建 QTimer(this)
            → connect(timeout → tick()) → m_timer->start(frameIntervalMs)
     │
     │  每次 tick() 读一帧 → enqueueFrame() 入队
     │
3. enqueueFrame — clone图像 → 打包成FramePacket → 放入m_frameQueue队尾
     │                                       │
     │  队列0→1时 emit frameAvailable() ────→ 通知算法线程"有货"
     │                                       │
4. tryDequeueFrame — 算法线程收到信号后调用，从m_frameQueue队头取走一帧

5. 自然读完 → m_running=false → 停止定时器 → emit finished()
   保留队列中的帧，由主窗口通知算法处理完后再清理。

6. 主动停止：stop() → m_running=false → m_timer->stop()
         → cap.release() → 清空队列 → emit finished()

Worker 随采集线程结束通过 deleteLater() 销毁；算法线程必须先结束。
*/
class FrameProducer : public QObject
{
    Q_OBJECT

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
    /**  @brief 生产者：启动采集循环（跨线程信号槽触发）
    内部流程：
    1. 根据 m_config.sourceType 走 AVI 或图片序列分支
    2. 打开源后发射 sourceInfoReady(路径, 分辨率)
    3. 创建 QTimer → connect(timeout → tick()) → timer->start(帧间隔)
    4. 采集结束（视频播完/图片读完）时 tick() 内部发射 finished()
    5. 异常发生时发射 errorOccurred(msg)*/
    void start();

    /// @brief 停止采集循环（跨线程信号槽触发）
    ///        等当前读取返回后执行，停止定时器并清空队列
    void stop();

private slots:
    /// @brief QTimer内部调用 每次触发时读一帧（AVI: cap.read, 图片: imread）
    ///        自然读完时停止采集并保留队列，发出 finished() 通知算法收尾
    void tick();

signals:
    /// @brief 帧队列从空变为非空时发射，用于唤醒算法线程取帧
    /// @note 仅在队列 0→1 的瞬间发射一次，避免高帧率下重复空唤醒
    void frameAvailable();

    /// @brief 自然读完或 stop() 执行后发出，不代表 QThread 已经退出
    void finished();

    /// @brief 采集过程中发生错误时发射
    /// @param message 可读的错误描述
    void errorOccurred(const QString& message);

    /// @brief 采集源打开成功后发射，供 UI 显示路径和分辨率
    /// @param path       输入源路径
    /// @param resolution 视频/首帧图像分辨率（宽×高）
    void sourceInfoReady(const QString& path, const QSize& resolution);

private:
    /** @brief 把start()读取到的帧数据放入FramePacket
    线程安全规则：
       - 入队前对 cv::Mat 执行 clone() 深拷贝
       - 队列满（≥ m_config.maxQueueSize）时丢弃队列头部最旧帧
       - 队列从空变为非空时，发射 frameAvailable() 信号
   
        @param frame       原始帧（只读，内部 clone 后入队）
        @param frameIdx    帧序号
        @param timestampMs 时间戳（毫秒）
        @return 是否成功入队（队列满导致丢帧时返回 false）*/
    bool enqueueFrame(const cv::Mat& frame, qint64 frameIdx, qint64 timestampMs);

    // ========================================================================
    // 成员变量
    // ========================================================================
    SourceConfig         m_config;           // 输入源配置（类型/路径/帧间隔/队列容量）
    QQueue<FramePacket>  m_frameQueue;       // 帧数据队列（单队列三合一，线程安全）
    QMutex               m_queueMutex;       // 保护 m_frameQueue 的互斥锁
    bool                 m_running = false;  // 仅采集线程使用；自然读完或 stop() 时置 false
    QStringList          m_imageFiles;       // 图片序列模式下的文件路径列表（已排序）
    int                  m_nextImageIndex = 0; // 图片序列模式下的下一张待读取索引
    QTimer*              m_timer = nullptr;  // 帧间隔定时器，随 Worker 在采集线程中销毁
    cv::VideoCapture     m_cap;              // AVI 视频采集器（stop() 时 release()）
};
