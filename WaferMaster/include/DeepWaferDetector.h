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
    /// @brief 初始化 ONNX Runtime 环境和会话
  bool loadModel(const std::string& modelPath);
    /// @brief 检查模型是否已加载
  bool isLoaded() const { return m_session != nullptr; }

    /// @brief 对一帧图像执行推理，返回完整的 AlgoResult
    /// @param frame      输入帧（BGR 彩色图或灰度图均可）
    /// @param frameIdx   帧序号（用于日志）
    /// @param timestampMs 时间戳
    AlgoResult processFrame(const cv::Mat& frame,qint64 frameIdx, qint64 timestampMs);

private:
    /// @brief 把 OpenCV 图像转成 ONNX 需要的格式(预处理)
    ///        灰度 → 3通道复制 → resize 256×256 → float[0,1] → NCHW
    cv::Mat preprocess(const cv::Mat& gray) const;

    /// @brief 异常分数 → Good/Warning/NG（后处理）
    DetectionLevel classifyScore(float score) const;

    // ---- ONNX Runtime 核心对象 ----

    // Ort::Env：首先构造ONNX Runtime 的运行环境（日志级别、线程数等）
    Ort::Env m_env{ORT_LOGGING_LEVEL_WARNING, "WaferMaster"};

    // Ort::MemoryInfo：描述内存分配策略（CPU 内存、连续分配）
    Ort::MemoryInfo m_memoryInfo{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

    // Ort::Session：代表一个已加载的模型。用 unique_ptr 是因为 Ort::Session 不
    // 支持默认构造，必须在 loadModel() 中真正加载模型后才创建
    std::unique_ptr<Ort::Session> m_session;
   
    DLconfig m_config; 
};