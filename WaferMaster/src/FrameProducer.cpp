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
    QMutexLocker locker(&m_queueMutex);//自动加锁，离开作用域自动解锁

    if (m_frameQueue.isEmpty())
        return false;

    packet = m_frameQueue.dequeue();//从队列中移除队头元素放入调用方的packet
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
        cap.open(m_config.sourcePath.toStdString());//把Qt 的 `QString` 转成 C++ 标准 `std::string`

        if (!cap.isOpened())
        {
            emit errorOccurred(QStringLiteral("无法打开视频文件：%1").arg(m_config.sourcePath));//Qt 的字符串格式化，%1 被替换成文件路径
            m_running = false;
            return;
        }

        // 发射源信息
        int w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));//get获取当前视频帧宽度的double
        int h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        emit sourceInfoReady(m_config.sourcePath, QSize(w, h));

        qint64 frameIdx = 0;
        cv::Mat frame;

        while (m_running)
        {
            cap.read(frame);
            if (frame.empty())
                break;   // 视频播完，正常退出（不是错误）

            double ts = cap.get(cv::CAP_PROP_POS_MSEC);//当前帧对应的时间戳（ms）
            enqueueFrame(frame, frameIdx, static_cast<qint64>(ts));//打包入队

            ++frameIdx;
            QThread::msleep(m_config.frameIntervalMs);//线程睡眠函数，让当前线程暂停指定的毫秒数
        }

        cap.release();// 关闭视频文件，释放资源
    }
    else
    {
        // ---- 图片序列分支 ----
        QDir dir(m_config.sourcePath);//QDir是 Qt 的目录操作类
        if (!dir.exists())
        {
            emit errorOccurred(QStringLiteral("目录不存在：%1").arg(m_config.sourcePath));
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
            emit errorOccurred(QStringLiteral("目录中无图片文件：%1").arg(m_config.sourcePath));
            m_running = false;
            return;
        }

        // 读首帧获取分辨率
        cv::Mat firstFrame = cv::imread(m_imageFiles.first().toStdString());
        if (!firstFrame.empty())
            emit sourceInfoReady(m_config.sourcePath, QSize(firstFrame.cols, firstFrame.rows));

        m_nextImageIndex = 0;//是计数器也是索引，记录下一张待读取的图片在 m_imageFiles 中的位置，从0开始递增

        while (m_running && m_nextImageIndex < m_imageFiles.size())//循环读取图片，直到 m_running 被置 false 或者读完所有图片
        {
            cv::Mat frame = cv::imread(m_imageFiles.at(m_nextImageIndex).toStdString());

            if (frame.empty())
            {
                // 单张图片读取失败：跳过，继续下一张（不中断采集）
                ++m_nextImageIndex;
                continue;
            }

            qint64 ts = QDateTime::currentMSecsSinceEpoch();//获取当前系统时间的时间戳（ms），模拟帧采集时的时间戳
            enqueueFrame(frame, m_nextImageIndex, ts);//m_nextImageIndex会int→ qint64隐式转换

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