#include "FrameProducer.h"

#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QMutexLocker>
#include <QThread>
#include <opencv2/opencv.hpp>
#include <spdlog/spdlog.h>

// ============================================================================
// 构造 / 析构
// ============================================================================

FrameProducer::FrameProducer()
    : QObject(nullptr)   // 无父对象，moveToThread 要求
{
}

FrameProducer::~FrameProducer()
{
    // 析构前应由 MainWindow 确保已调用 stop()
}

// ============================================================================
// 公开接口
// ============================================================================

void FrameProducer::setSourceConfig(const SourceConfig& config)
{
    m_config = config;
}

bool FrameProducer::tryDequeueFrame(FramePacket& packet)
{
    QMutexLocker locker(&m_queueMutex);//自动加锁，离开作用域自动解锁，与 `enqueueFrame` 共享同一把 `m_queueMutex`

    if (m_frameQueue.isEmpty())
        return false;

    packet = m_frameQueue.dequeue();//从队列中移除队头元素放入调用方的packet
    return true;
}

// ============================================================================
// 槽函数：start() — 打开源 + 启动 QTimer
// ============================================================================

void FrameProducer::start()
{
    m_running = true;

    Logger::get()->info("Producer started, source: {}, path: {}",
        m_config.sourceType == InputSourceType::AviVideo ? "AVI" : "ImageSequence",
        m_config.sourcePath.toStdString());

    if (m_config.sourceType == InputSourceType::AviVideo)
    {
        // ---- AVI 视频分支 ----
        m_cap.open(m_config.sourcePath.toStdString());//把Qt 的 `QString` 转成 C++ 标准 `std::string`

        if (!m_cap.isOpened())
        {
            Logger::get()->error("Cannot open video file: {}", m_config.sourcePath.toStdString());
            emit errorOccurred(QStringLiteral("Cannot open video file: %1").arg(m_config.sourcePath));
            m_running = false;
            return;
        }

        // 发射源信息
        int w = static_cast<int>(m_cap.get(cv::CAP_PROP_FRAME_WIDTH));
        int h = static_cast<int>(m_cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        emit sourceInfoReady(m_config.sourcePath, QSize(w, h));
    }
    else
    {
        // ---- 图片序列分支 ----
        QDir dir(m_config.sourcePath);//QDir是 Qt 的目录操作类
        if (!dir.exists())
        {
            emit errorOccurred(QStringLiteral("Directory not found: %1").arg(m_config.sourcePath));
            m_running = false;
            return;
        }

        // 收集图片文件并按名称排序
        QStringList filters;//Qt 的字符串列表类型,类似std::vector<QString>
        filters << QStringLiteral("*.bmp") << QStringLiteral("*.jpg")
                << QStringLiteral("*.jpeg") << QStringLiteral("*.png")
                << QStringLiteral("*.tiff") << QStringLiteral("*.tif");
        m_imageFiles = dir.entryList(filters, QDir::Files, QDir::Name);//获取目录下所有符合过滤条件的文件，并按名称排序

        // 补全为绝对路径
        for (QString& f : m_imageFiles)
            f = dir.absoluteFilePath(f);

        if (m_imageFiles.isEmpty())
        {
            emit errorOccurred(QStringLiteral("No image files in: %1").arg(m_config.sourcePath));
            m_running = false;
            return;
        }

        // 读首帧获取分辨率
        cv::Mat firstFrame = cv::imread(m_imageFiles.first().toStdString());
        if (!firstFrame.empty())
            emit sourceInfoReady(m_config.sourcePath, QSize(firstFrame.cols, firstFrame.rows));

        m_nextImageIndex = 0;//是计数器也是索引，记录下一张待读取的图片在 m_imageFiles 中的位置，从0开始递增
    }

    // 创建 QTimer（this 作为父对象）
    m_timer = new QTimer(this);
    m_timer->start(m_config.frameIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &FrameProducer::tick);
    //启动定时器，每 33ms 发射一次 timeout 信号，触发一次 tick()
}

// ============================================================================
// 私有槽：tick() — QTimer 每次触发时读一帧
// ============================================================================

void FrameProducer::tick()
{
    if (!m_running)
    {
        // 被 stop() 抢先设 false，直接忽略本次 tick
        return;
    }

    cv::Mat frame;
    qint64 ts = 0;
    qint64 frameIdx = 0;

    if (m_config.sourceType == InputSourceType::AviVideo)
    {
        // ---- AVI 视频：读一帧 ----
        m_cap.read(frame);//读帧自动存入frame
        if (frame.empty())
        {
            // 视频播放完毕，自然结束
            m_timer->stop();
            m_cap.release();
            emit finished();
            return;
        }
        ts = m_cap.get(cv::CAP_PROP_POS_MSEC);
        frameIdx = m_nextImageIndex; // AVI 也用递增计数器
        ++m_nextImageIndex;
    }
    else
    {
        // ---- 图片序列：读一张 ----
        if (m_nextImageIndex >= m_imageFiles.size())
        {
            // 全部读完，自然结束
            m_timer->stop();
            emit finished();
            return;
        }

        frame = cv::imread(m_imageFiles.at(m_nextImageIndex).toStdString());

        if (frame.empty())
        {
            // 单张图片读取失败：跳过，继续下一张
            ++m_nextImageIndex;
            return;
        }

        ts = QDateTime::currentMSecsSinceEpoch();//图片序列没有内置时间戳，使用当前系统时间模拟
        frameIdx = m_nextImageIndex;
        ++m_nextImageIndex;
    }

    enqueueFrame(frame, frameIdx, ts);//把读到的帧数据放入队列，供算法线程取用
}

// ============================================================================
// 槽函数：stop() — 立即停止 QTimer，释放资源
// ============================================================================

void FrameProducer::stop()
{
    m_running = false;

    // 停止定时器——后续 tick 不会被调度
    if (m_timer)
    {
        m_timer->stop();
        // 不 delete，start() 会重建；stop() 后 delete Worker 时自动析构
    }

    // 释放视频采集器
    if (m_cap.isOpened())
        m_cap.release();

    // 清空队列
    {
        QMutexLocker locker(&m_queueMutex);
        m_frameQueue.clear();
    }

    // 清空图片列表
    m_imageFiles.clear();

    Logger::get()->info("Producer stopped, total frames: {}", m_nextImageIndex);
    emit finished();
}

// ============================================================================
// 私有方法：enqueueFrame()
// ============================================================================

bool FrameProducer::enqueueFrame(const cv::Mat& frame, qint64 frameIdx, qint64 timestampMs)
{
    QMutexLocker locker(&m_queueMutex);

    // 队列满时丢弃最旧帧
    while (m_frameQueue.size() >= m_config.maxQueueSize)
        m_frameQueue.dequeue();//丢弃队头多余的帧

    // clone 深拷贝后入队
    FramePacket packet;
    packet.frame       = frame.clone();
    packet.frameIdx    = frameIdx;
    packet.timestampMs = timestampMs;//将start()采集的3个原始数据装进FramePacket结构体。
    m_frameQueue.enqueue(packet);//将打包好的数据放入队尾

    // 队列从空变为非空（0→1）时发射信号，唤醒算法线程
    if (m_frameQueue.size() == 1)
        emit frameAvailable();

    return true;
}