#pragma once

#include <opencv2/opencv.hpp>
#include <onnxruntime_cxx_api.h>
#include "Common.h"

/// @brief PatchCore ONNX 推理器
///        在算法线程内同步调用，不继承 QObject（不需要信号槽）
class DeepWaferDetector 
{
public:
    DeepWaferDetector();
    ~DeepWaferDetector();

    ///@brief 注入深度学习配置（阈值、输入尺寸）
  void setDLconfig(const DLconfig& config) { m_config = config; };
    /// @brief 加载模型并检查接口；QString 路径可正确转换为 Windows 宽字符串
    /// @return 成功返回 true；ONNX Runtime 加载错误返回 false
    /// @note 配置或接口不兼容时抛出 std::runtime_error，由 WaferAlgorithm::start() 捕获
  bool loadModel(const QString& modelPath);
    /// @brief 检查模型是否已加载
  bool isLoaded() const { return m_session != nullptr; }

    /// @brief 对一帧图像执行推理，成功时返回完整的 AlgoResult
    /// @note 失败时抛出异常，由 WaferAlgorithm::processPendingFrames() 报错并停止检测
    /// @param frame      输入帧（BGR 彩色图或灰度图均可）
    /// @param frameIdx   帧序号（用于日志）
    /// @param timestampMs 时间戳
    AlgoResult processFrame(const cv::Mat& frame,qint64 frameIdx, qint64 timestampMs);

private:
    /// @brief 把 OpenCV 图像转成 ONNX 需要的格式(预处理)
    ///        BGR 原样或灰度复制为 3 通道 → resize 到 inputSize → float[0,1] → NCHW
    cv::Mat preprocess(const cv::Mat& gray) const;

    /// @brief 异常分数 → Good/Warning/NG（后处理）
    DetectionLevel classifyScore(float score) const;

    // ---- ONNX Runtime 核心对象 ----

    // Ort::Env：首先构造ONNX Runtime 的运行环境（日志级别、线程数等）
    Ort::Env m_env{ORT_LOGGING_LEVEL_WARNING, "WaferMaster"};

    // Ort::MemoryInfo：描述内存分配策略（CPU 内存、连续分配）
    Ort::MemoryInfo m_memoryInfo{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

    // Ort::Session：代表一个已加载的模型。unique_ptr 在加载前可以为空，
    // 加载成功后持有会话，并在 reset() 或检测器销毁时自动释放会话。
    std::unique_ptr<Ort::Session> m_session;
   
    DLconfig m_config; 
};
