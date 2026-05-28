#include "FrameProducer.h"

#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QMutexLocker>
#include <QThread>
#include <opencv2/opencv.hpp>

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
    QMutexLocker locker(&m_queueMutex);

    if (m_frameQueue.isEmpty())
        return false;

    packet = m_frameQueue.dequeue();
    return true;
}

// ============================================================================
// 槽函数：start() — 采集循环
// ============================================================================

void FrameProducer::start()
{
    m_running = true;

    if (m_config.sourceType == InputSourceType::AviVideo)
    {
        // ---- AVI 视频分支 ----
        cv::VideoCapture cap;
        cap.open(m_config.sourcePath.toStdString());

        if (!cap.isOpened())
        {
            emit errorOccurred(QStringLiteral("无法打开视频文件：%1").arg(m_config.sourcePath));
            m_running = false;
            return;
        }

        // 发射源信息
        int w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        int h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        emit sourceInfoReady(m_config.sourcePath, QSize(w, h));

        qint64 frameIdx = 0;
        cv::Mat frame;

        while (m_running)
        {
            cap.read(frame);
            if (frame.empty())
                break;   // 视频播完，正常退出（不是错误）

            double ts = cap.get(cv::CAP_PROP_POS_MSEC);
            enqueueFrame(frame, frameIdx, static_cast<qint64>(ts));

            ++frameIdx;
            QThread::msleep(m_config.frameIntervalMs);
        }

        cap.release();
    }
    else
    {
        // ---- 图片序列分支 ----
        QDir dir(m_config.sourcePath);
        if (!dir.exists())
        {
            emit errorOccurred(QStringLiteral("目录不存在：%1").arg(m_config.sourcePath));
            m_running = false;
            return;
        }

        // 收集图片文件并按名称排序
        QStringList filters;
        filters << QStringLiteral("*.bmp") << QStringLiteral("*.jpg")
                << QStringLiteral("*.jpeg") << QStringLiteral("*.png")
                << QStringLiteral("*.tiff") << QStringLiteral("*.tif");
        m_imageFiles = dir.entryList(filters, QDir::Files, QDir::Name);

        // 补全为绝对路径
        for (QString& f : m_imageFiles)
            f = dir.absoluteFilePath(f);

        if (m_imageFiles.isEmpty())
        {
            emit errorOccurred(QStringLiteral("目录中无图片文件：%1").arg(m_config.sourcePath));
            m_running = false;
            return;
        }

        // 读首帧获取分辨率
        cv::Mat firstFrame = cv::imread(m_imageFiles.first().toStdString());
        if (!firstFrame.empty())
            emit sourceInfoReady(m_config.sourcePath, QSize(firstFrame.cols, firstFrame.rows));

        m_nextImageIndex = 0;

        while (m_running && m_nextImageIndex < m_imageFiles.size())
        {
            cv::Mat frame = cv::imread(m_imageFiles.at(m_nextImageIndex).toStdString());

            if (frame.empty())
            {
                // 单张图片读取失败：跳过，继续下一张（不中断采集）
                ++m_nextImageIndex;
                continue;
            }

            qint64 ts = QDateTime::currentMSecsSinceEpoch();
            enqueueFrame(frame, m_nextImageIndex, ts);

            ++m_nextImageIndex;
            QThread::msleep(m_config.frameIntervalMs);
        }
    }

    // 正常结束（非错误退出）
    emit finished();
    m_running = false;
}

// ============================================================================
// 槽函数：stop()
// ============================================================================

void FrameProducer::stop()
{
    m_running = false;   // 循环检测到 false 后自然退出
}

// ============================================================================
// 私有方法：enqueueFrame()
// ============================================================================

bool FrameProducer::enqueueFrame(const cv::Mat& frame, qint64 frameIdx, qint64 timestampMs)
{
    QMutexLocker locker(&m_queueMutex);

    // 队列满时丢弃最旧帧
    while (m_frameQueue.size() >= m_config.maxQueueSize)
        m_frameQueue.dequeue();

    // clone 深拷贝后入队
    FramePacket packet;
    packet.frame       = frame.clone();
    packet.frameIdx    = frameIdx;
    packet.timestampMs = timestampMs;
    m_frameQueue.enqueue(packet);

    // 队列从空变为非空（0→1）时发射信号，唤醒算法线程
    if (m_frameQueue.size() == 1)
        emit frameAvailable();

    return true;
}