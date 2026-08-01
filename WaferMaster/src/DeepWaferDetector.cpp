#include "DeepWaferDetector.h"
#include "Logger.h"
#include <chrono>
#include <cstring>

// ============================================================================
// 构造 & 析构
// ============================================================================

DeepWaferDetector::DeepWaferDetector()
{
}
DeepWaferDetector::~DeepWaferDetector()
{
    //unique_ptr自动释放
}
bool DeepWaferDetector::loadModel(const std::string& modelPath)
{
    try
    {
        // 创建会话配置
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(2);                          // CPU 线程数
        opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);        // 图优化级别

        // 用配置创建 Session（= 加载模型文件到内存）
        m_session = std::make_unique<Ort::Session>(
            m_env,              // 运行环境（整个程序共享一个）
            modelPath.c_str(),  // 文件路径
            opts);              
         Logger::get()->info("Loaded model from {}", modelPath);

        return true;
    }
    catch (const Ort::Exception& e) {
        // 文件不存在、格式损坏、内存不足触发
        Logger::get()->error("Failed to load model: {}", e.what());
        return false;
    }
}

cv::Mat DeepWaferDetector::preprocess(const cv::Mat& gray) const
{
  cv::Mat  rgb;
  cv::cvtColor(gray,rgb,cv::COLOR_GRAY2RGB); // 灰度 → 3通道RGB
  
  cv::Mat resized;
  cv::resize(rgb, resized, cv::Size(m_config.inputSize, m_config.inputSize)); // resize 256×256   
  resized.convertTo(resized, CV_32F, 1.0 / 255.0); // float[0,1]

  cv::Mat nchw;
  cv::dnn::blobFromImage(resized, nchw);//OpenCV 存图：H×W×C（高×宽×通道） → NCHW（批×通道×高×宽）
    return nchw;
}

DetectionLevel DeepWaferDetector::classifyScore(float score) const
{
    if(score < m_config.goodThreshold)// score < 0.54
        return DetectionLevel::Good;
    if(score < m_config.ngThreshold)// 0.54 <= score < 0.7231
         return DetectionLevel::Warning;
        //(else) score >= 0.7231
         return DetectionLevel::Ng;
}

AlgoResult DeepWaferDetector::processFrame(const cv::Mat& frame,qint64 frameIdx, qint64 timestampMs)
{
    //创建结果容器
    AlgoResult result;
    result.frameIdx    = frameIdx;
    result.timestampMs = timestampMs;
    result.mode        = DetectionMode::DeepLearning;
    result.fi = result.p95 = result.hotRatio = -1.0; // DL 模式下不计算传统指标，-1 表示无效

    if(!isLoaded())
    {
        Logger::get()->error("Model not loaded, cannot process frame #{}", frameIdx);
        return result;
    }

    // 统一转灰度
    cv::Mat gray;
    if (frame.channels() == 3)
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    else
        gray = frame;

  auto t0 = std::chrono::steady_clock::now();// 计时：推理开始

try
{
  // ══════════════════════════════════════════════
    //ONNX Runtime 推理
  // ══════════════════════════════════════════════

    //1 预处理：灰度到 NCHW
    cv::Mat nchw=preprocess(gray);

    //2 打包成onnx认识的输入张量
    std::array<int64_t,4> intputshape{1,3,m_config.inputSize,m_config.inputSize};
    auto inputTensor = Ort::Value::CreateTensor<float>//手动创建张量容器Ort::Value
    (
        m_memoryInfo,               // 内存分配策略（CPU、连续）
         nchw.ptr<float>(),         // 数据指针
          nchw.total(),             // 元素总数（= 1×3×256×256）
           intputshape.data(),      //形状[1,3,256,256]
            intputshape.size());    // 形状维度数=4

    //3 推理←→ 把数据喂给模型，取回分数和热力图
    const char* inputnames[] = {"input"}; // 输入节点名
    const char* outputnames[] = {"pred_score", "anomaly_map"}; // 输出节点名（与模型预设必须一致！）
    auto outputs =m_session->Run                     //创建一堆张量的集合std::vector<Ort::Value>
    (Ort::RunOptions{nullptr}, inputnames, &inputTensor, 1, outputnames, 2);//outputs.size() == 2
    
  // ══════════════════════════════════════════════
     //拆包结果
  // ══════════════════════════════════════════════    
    
   float score = outputs[0].GetTensorData<float>()[0];//拆包分数
   result.anomalyScore=score;
   result.level=classifyScore(score);//判定等级

  //拆包热力图,并转成cv::Mat
   auto& heatmapTensor=outputs[1];
   auto heatmapShape=heatmapTensor.GetTensorTypeAndShapeInfo().GetShape();//返回形状[1,1,256,256]
   int mapH=static_cast<int>(heatmapShape[2]);
   int mapW=static_cast<int>(heatmapShape[3]);
   cv::Mat rawheatmap(mapH,mapW,CV_32FC1);
   std::memcpy(
   rawheatmap.data, heatmapTensor.GetTensorData<float>(), mapH*mapW*sizeof(float));//深拷贝数据到cv::Mat

  //热力图归一化+伪彩色
    cv::Mat heat8u;
    cv::normalize(rawheatmap, heat8u, 0, 255, cv::NORM_MINMAX);
    heat8u.convertTo(heat8u, CV_8UC1);

    cv::Mat heatColor;
    cv::applyColorMap(heat8u, heatColor, cv::COLORMAP_JET);
    cv::resize(heatColor, heatColor, gray.size());

  //填充三视图
    result.frameOriginal = frame.clone();           // 左上：原图
    result.frameSpectrum = heatColor.clone();       // 右上：热力图 
    cv::Mat originalColor;
    cv::cvtColor(gray, originalColor, cv::COLOR_GRAY2BGR);
    cv::addWeighted(originalColor,0.6,   // 第一张：原图，权重 60%
                    heatColor,    0.4,   // 第二张：热力图，权重 40%
                    0,                      // 亮度偏移，一般写 0
                    result.frameFlatness);  // 输出到第三个视图
    
}
catch(const std::exception& e)
{
    Logger::get()->error("DL inference failed frame #{}: {}", frameIdx, e.what());
}

   auto t1 = std::chrono::steady_clock::now();// 计时：推理结束
   result.inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
   
   Logger::get()->debug("DL Frame #{}: score={:.3f}, level={}, {}ms",
    frameIdx, result.anomalyScore,
    detectionLevelToString(result.level).toStdString(),
    result.inferenceTimeMs);

   return result;

}
