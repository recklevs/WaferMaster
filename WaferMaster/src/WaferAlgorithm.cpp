#include "WaferAlgorithm.h"
#include "FrameProducer.h"

#include <opencv2/opencv.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <vector>
#include <cmath>
#include <chrono>

// ============================================================================
// 构造 / 析构
// ============================================================================

WaferAlgorithm::WaferAlgorithm()
    : QObject(nullptr)
{
}

WaferAlgorithm::~WaferAlgorithm()
{
}

// ============================================================================
// 配置注入
// ============================================================================

void WaferAlgorithm::setAlgoConfig(const AlgoConfig& config)
{
    m_config = config;
}

void WaferAlgorithm::setFrameProducer(FrameProducer* producer)
{
    m_producer = producer;
}

// ============================================================================
// 公有槽
// ============================================================================

void WaferAlgorithm::start()
{
    m_running = true;
}

void WaferAlgorithm::processPendingFrames()
{
    if (!m_producer)
    {
        emit errorOccurred(QStringLiteral("FrameProducer not set"));
        return;
    }

    FramePacket packet;
    while (m_running)
    {
        if (!m_producer->tryDequeueFrame(packet))
            break;   // 队列空，直接返回

        auto t0 = std::chrono::steady_clock::now();
        AlgoResult result = processSingleFrame(packet.frame, packet.frameIdx, packet.timestampMs);
        auto t1 = std::chrono::steady_clock::now();

        auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        Logger::get()->debug("Frame #{} processed in {} us, FI={:.2f}, HotRatio={:.2f}%, Level={}",
            packet.frameIdx, elapsedUs, result.fi, result.hotRatio,
            detectionLevelToString(result.level).toStdString());

        if (result.level == DetectionLevel::Ng)
            Logger::get()->warn("NG Frame #{}: FI={:.2f}, P95={:.2f}, HotRatio={:.2f}%",
                packet.frameIdx, result.fi, result.p95, result.hotRatio);

        emit resultReady(result);
    }
}

void WaferAlgorithm::stop()
{
    m_running = false;
    emit finished();
}

// ============================================================================
// 私有方法：算法主链
// ============================================================================

AlgoResult WaferAlgorithm::processSingleFrame(const cv::Mat& frame, qint64 frameIdx, qint64 timestampMs) const
{
    AlgoResult result;
    result.frameIdx    = frameIdx;
    result.timestampMs = timestampMs;

    try
    {
    // 保存原图副本（UI 显示用，clone 确保独立所有权）
    result.frameOriginal = frame.clone();

    // --- 步骤1：灰度转换 ---
    cv::Mat gray;
    if (frame.channels() == 3)
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    else
        gray = frame.clone();

    // 转为 32 位浮点，为后续 DFT 做准备
    cv::Mat gray32f;
    gray.convertTo(gray32f, CV_32F);

    // --- 步骤2：warpPolar 极坐标变换 ---
    cv::Point2f center(static_cast<float>(gray32f.cols) / 2.0f,
                       static_cast<float>(gray32f.rows) / 2.0f);
    double maxRadius = std::min(center.x, center.y);
    cv::Mat polarImg;
    cv::warpPolar(gray32f, polarImg, cv::Size(frame.cols, frame.rows), center,
                  maxRadius, cv::INTER_LINEAR + cv::WARP_POLAR_LINEAR);

    // --- 步骤3：DFT 傅里叶变换 ---
    int m = cv::getOptimalDFTSize(polarImg.rows);
    int n = cv::getOptimalDFTSize(polarImg.cols);
    cv::Mat padded;
    cv::copyMakeBorder(polarImg, padded, 0, m - polarImg.rows, 0,
                       n - polarImg.cols, cv::BORDER_CONSTANT, cv::Scalar::all(0));

    cv::Mat planes[2] = { cv::Mat_<float>(padded), cv::Mat::zeros(padded.size(), CV_32F) };
    cv::Mat complexI;
    cv::merge(planes, 2, complexI);

    cv::dft(complexI, complexI);

    // --- 步骤4：频谱中心化 + 对数增强（用于可视化） ---
    shiftDFT(complexI);

    cv::split(complexI, planes);
    cv::Mat magnitudeImg;
    cv::magnitude(planes[0], planes[1], magnitudeImg);
    magnitudeImg += cv::Scalar::all(1);
    cv::log(magnitudeImg, magnitudeImg);
    cv::normalize(magnitudeImg, magnitudeImg, 0, 255, cv::NORM_MINMAX);
    magnitudeImg.convertTo(result.frameSpectrum, CV_8UC1);

    // --- 步骤5：圆环带通掩膜 ---
    cv::Mat mask = cv::Mat::zeros(complexI.size(), CV_32F);
    cv::Point maskCenter(complexI.cols / 2, complexI.rows / 2);
    int minSide = std::min(complexI.rows, complexI.cols);
    int r_inner = static_cast<int>(minSide * m_config.bandPassInnerRatio);
    int r_outer = static_cast<int>(minSide * m_config.bandPassOuterRatio);

    cv::circle(mask, maskCenter, r_outer, cv::Scalar(1), -1);
    cv::circle(mask, maskCenter, r_inner, cv::Scalar(0), -1);

    cv::Mat maskPlanes[2] = { mask, mask };
    cv::Mat complexMask;
    cv::merge(maskPlanes, 2, complexMask);

    cv::multiply(complexI, complexMask, complexI);

    // --- 步骤6：IDFT 逆变换 ---
    shiftDFT(complexI);
    cv::Mat filtered;
    cv::idft(complexI, filtered, cv::DFT_SCALE | cv::DFT_REAL_OUTPUT);

    // --- 步骤7：算法 ROI 统计 ---
    QRect roiQ = makeCentralRoi(QSize(filtered.cols, filtered.rows));
    cv::Rect roiRect(roiQ.x(), roiQ.y(), roiQ.width(), roiQ.height());
    result.algoRoiRect = roiQ;

    cv::Mat filteredRoi = filtered(roiRect);

    // FI 平坦度：ROI 内标准差
    cv::Scalar meanValue, stdValue;
    cv::meanStdDev(filteredRoi, meanValue, stdValue);
    result.fi = stdValue[0];

    // P95：第 95 百分位像素强度
    result.p95 = percentile95(filteredRoi);

    // HotRatio：热点像素占比
    cv::Mat hotMask = filteredRoi > m_config.p95Threshold;
    result.hotRatio = 100.0 * static_cast<double>(cv::countNonZero(hotMask)) /
                      static_cast<double>(roiRect.width * roiRect.height);

    // --- 步骤8：classify 判定 ---
    result.level = classify(result.fi, result.p95, result.hotRatio);

    // --- 步骤9：生成平坦图（可视化结果） ---
    cv::Mat flatness;
    cv::normalize(filtered, flatness, 0, 255, cv::NORM_MINMAX);
    flatness.convertTo(flatness, CV_8U);
    cv::cvtColor(flatness, flatness, cv::COLOR_GRAY2BGR);

    // 绘制算法 ROI 矩形框（颜色由等级决定）
    cv::Scalar levelColor;
    switch (result.level)
    {
    case DetectionLevel::Good:    levelColor = cv::Scalar(0, 255, 0);   break; // 绿
    case DetectionLevel::Warning: levelColor = cv::Scalar(0, 255, 255); break; // 黄
    case DetectionLevel::Ng:      levelColor = cv::Scalar(0, 0, 255);   break; // 红
    }
    cv::rectangle(flatness, roiRect, levelColor, 2);

    // 绘制文字信息
    std::ostringstream text1, text2, text3;
    text1 << std::fixed << std::setprecision(2) << "FI: " << result.fi;
    text2 << std::fixed << std::setprecision(2) << "HotRatio: " << result.hotRatio << "%";
    text3 << "Level: " << detectionLevelToString(result.level).toStdString();
    int textX = std::max(10, flatness.cols - 320);
    cv::putText(flatness, text1.str(), cv::Point(textX, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, levelColor, 2);
    cv::putText(flatness, text2.str(), cv::Point(textX, 60),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, levelColor, 2);
    cv::putText(flatness, text3.str(), cv::Point(textX, 90),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, levelColor, 2);

    result.frameFlatness = flatness;

    }
    catch (const std::exception& e)
    {
        Logger::get()->error("Frame #{} algorithm exception: {}", frameIdx, e.what());
        // 返回部分填充的结果（frameOriginal 已 clone 在外层捕获前）
    }

    return result;
}

// ============================================================================
// 私有方法：工具函数（按 processSingleFrame 内部调用序排列）
// ============================================================================

void WaferAlgorithm::shiftDFT(cv::Mat& fImage) const
{
    int cx = fImage.cols / 2;
    int cy = fImage.rows / 2;

    cv::Mat q0(fImage, cv::Rect(0, 0, cx, cy));       // 左上
    cv::Mat q1(fImage, cv::Rect(cx, 0, cx, cy));      // 右上
    cv::Mat q2(fImage, cv::Rect(0, cy, cx, cy));      // 左下
    cv::Mat q3(fImage, cv::Rect(cx, cy, cx, cy));     // 右下

    cv::Mat tmp;
    q0.copyTo(tmp);  q3.copyTo(q0);  tmp.copyTo(q3);  // q0 <-> q3
    q1.copyTo(tmp);  q2.copyTo(q1);  tmp.copyTo(q2);  // q1 <-> q2
}

QRect WaferAlgorithm::makeCentralRoi(const QSize& size) const
{
    int w = std::max(1, static_cast<int>(std::round(size.width()  * m_config.roiRatioW)));
    int h = std::max(1, static_cast<int>(std::round(size.height() * m_config.roiRatioH)));
    int x = std::max(0, (size.width()  - w) / 2);
    int y = std::max(0, (size.height() - h) / 2);// // 计算 ROI 左上角坐标，使其居中

    // 边界保护
    if (x + w > size.width())  w = size.width()  - x;
    if (y + h > size.height()) h = size.height() - y;

    return QRect(x, y, w, h);
}

float WaferAlgorithm::percentile95(const cv::Mat& roi32f) const
{
    // 将 ROI 内所有像素值拷贝到 vector
    std::vector<float> values;
    values.reserve(roi32f.rows * roi32f.cols);//预分配内存

    for (int r = 0; r < roi32f.rows; ++r)
    {
        const float* row = roi32f.ptr<float>(r);//按 float 类型访问矩阵数据，获取第 r 行的起始地址，并把它存到一个只读指针里
        for (int c = 0; c < roi32f.cols; ++c)
        {
            values.push_back(row[c]); // 逐像素拷贝
        }
    }

    if (values.empty())
        return 0.0f;

    // 计算第 95 百分位索引
    size_t idx = static_cast<size_t>(values.size() * 0.95);
    if (idx >= values.size())
        idx = values.size() - 1;

    // 部分排序：将第 idx 个元素放到它应在的位置
    std::nth_element(values.begin(), values.begin() + idx, values.end());

    return values[idx];
}

DetectionLevel WaferAlgorithm::classify(double fi, double p95, double hotRatio) const
{
    // 三指标各自与阈值比较：超出计 1 分
    int score = (fi >= m_config.fiThreshold) +
                (p95 >= m_config.p95Threshold) +
                (hotRatio >= m_config.hotRatioThreshold);

    switch (score)
    {
    case 0:  return DetectionLevel::Good;    // 三项全通过
    case 3:  return DetectionLevel::Ng;       // 三项全超
    default: return DetectionLevel::Warning;  // 1~2 项超阈值
    }
}
