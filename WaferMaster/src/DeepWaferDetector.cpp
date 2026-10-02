#include "DeepWaferDetector.h"
#include "Logger.h"
#include <chrono>
#include <cmath>      // std::isfinite：检查是否为有限数，排除 NaN 和无穷大
#include <cstring>
#include <limits>     // std::numeric_limits：查询 int 等类型能表示的最大值
#include <stdexcept>  // std::runtime_error：检查失败时携带错误原因并抛出异常

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
/*
 * 加载流程：检查配置 → 创建临时 Session → 检查模型接口 → 保存 Session。
 * Session 是 ONNX Runtime 中代表“已加载模型”的对象。
 * 加载成功返回 true；ONNX Runtime 的加载错误在这里记录并返回 false。
 * 本程序主动抛出的 std::runtime_error 会交给 WaferAlgorithm::start() 处理。
 */
bool DeepWaferDetector::loadModel(const QString& modelPath)
{
    // reset() 释放原有会话并置空，避免重新加载失败后仍被当作“模型已加载”。
    m_session.reset();
    try
    {
        // 输入尺寸用于后面的 resize 和张量形状，必须大于 0。
        // throw 会立即退出当前处理路径；调用者捕获异常后停止检测、显示错误。
        if (m_config.inputSize <= 0)
            throw std::runtime_error("DL inputSize must be positive");

        // 创建会话配置
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(2);                          // CPU 线程数
        opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);        // 图优化级别

        // Windows 的 ONNX Runtime 使用 wchar_t* 路径。
        const std::wstring wpath = modelPath.toStdWString();
        // 暂时放在局部智能指针中，全部检查通过后才交给 m_session。
        // 如果检查抛出异常，局部 session 会自动释放，m_session 仍为空。
        auto session = std::make_unique<Ort::Session>(m_env, wpath.c_str(), opts);

        // 1. 检查输入接口：本程序只会提供一个名为 input 的图像张量。
        // allocator 用来分配节点名字符串；返回的智能指针会自动释放该字符串。
        Ort::AllocatorWithDefaultOptions allocator;
        if (session->GetInputCount() != 1)
            throw std::runtime_error("ONNX model must have exactly one input");
        auto inputName = session->GetInputNameAllocated(0, allocator);
        // get() 取出 char*；strcmp() 比较字符串内容，返回 0 表示相同。
        if (std::strcmp(inputName.get(), "input") != 0)
            throw std::runtime_error("ONNX input node 'input' not found");
        auto inputType = session->GetInputTypeInfo(0);
        // 先确认它是张量，再查询张量的数据类型和形状。
        if (inputType.GetONNXType() != ONNX_TYPE_TENSOR)
            throw std::runtime_error("ONNX input must be a float32 tensor");
        auto inputTensorInfo = inputType.GetTensorTypeAndShapeInfo();
        auto inputShape = inputTensorInfo.GetShape();
        // 需要 float32 的 NCHW：[图像数量, 通道数, 高, 宽] = [1,3,inputSize,inputSize]。
        // 模型声明中的 -1 表示动态维度，具体大小由运行时输入决定，也允许通过。
        // || 从左到右短路判断：不是 4 维就停止检查，避免访问不存在的 shape[3]。
        if (inputTensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            inputShape.size() != 4 ||
            (inputShape[0] != 1 && inputShape[0] != -1) ||
            (inputShape[1] != 3 && inputShape[1] != -1) ||
            (inputShape[2] != m_config.inputSize && inputShape[2] != -1) ||
            (inputShape[3] != m_config.inputSize && inputShape[3] != -1))
            throw std::runtime_error("ONNX input must be float32 [1,3,inputSize,inputSize]");

        // 2. 查找本程序需要的两个输出：分数 pred_score、热力图 anomaly_map。
        // 遍历并按名字识别，因此模型输出顺序可以变化，也允许存在其他输出。
        bool hasScore = false;
        bool hasMap = false;
        for (size_t i = 0; i < session->GetOutputCount(); ++i)
        {
            auto name = session->GetOutputNameAllocated(i, allocator);
            const bool score = std::strcmp(name.get(), "pred_score") == 0;
            const bool map = std::strcmp(name.get(), "anomaly_map") == 0;
            if (!score && !map)
                continue; // 本程序不用的输出，跳过即可

            // 后面按 float 读取数据，所以这两个输出必须都是 float32 张量。
            auto type = session->GetOutputTypeInfo(i);
            if (type.GetONNXType() != ONNX_TYPE_TENSOR)
                throw std::runtime_error("ONNX outputs must be float32 tensors");
            auto tensorInfo = type.GetTensorTypeAndShapeInfo();
            if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                throw std::runtime_error("ONNX outputs must be float32 tensors");
            auto shape = tensorInfo.GetShape();
            if (score)
            {
                // 分数只需一个数：[1]、[1,1] 或标量均可；动态维度先允许通过。
                // 这里检查模型声明，实际元素个数会在 processFrame() 中再次确认。
                for (int64_t dimension : shape)
                    if (dimension != 1 && dimension != -1)
                        throw std::runtime_error("ONNX pred_score must contain one value");
                hasScore = true;
            }
            if (map)
            {
                // 热力图约定为 [1,1,H,W]，即一张单通道图。
                // H/W 允许任意正数或动态维度，不能固定要求为 256，便于以后换模型。
                if (shape.size() != 4 ||
                    (shape[0] != 1 && shape[0] != -1) ||
                    (shape[1] != 1 && shape[1] != -1) ||
                    (shape[2] <= 0 && shape[2] != -1) ||
                    (shape[3] <= 0 && shape[3] != -1))
                    throw std::runtime_error("ONNX anomaly_map must be [1,1,H,W]");
                hasMap = true;
            }
        }
        // 两个必需输出都找到才算兼容，不能把只含其中一个输出的模型当作可用。
        if (!hasScore || !hasMap)
            throw std::runtime_error("ONNX outputs 'pred_score' and 'anomaly_map' are required");

        // std::move 转移 unique_ptr 的所有权，不是复制整份模型。
        // 从这里开始 isLoaded() 才会返回 true。
        m_session = std::move(session);
        Logger::get()->info("Loaded model from {}", modelPath.toUtf8().toStdString());

        return true;
    }
    catch (const Ort::Exception& e) {
        // ONNX Runtime 自己报告的错误：记录原始原因，返回 false 让 start() 停止检测。
        Logger::get()->error("Failed to load model: {}", e.what());
        return false;
    }
}

cv::Mat DeepWaferDetector::preprocess(const cv::Mat& input) const
{
    cv::Mat rgb;
    if (input.channels() == 3)
        rgb = input; // 保持当前 BGR 顺序；变量名 rgb 并不表示这里进行了 BGR→RGB 转换
    else
        cv::cvtColor(input, rgb, cv::COLOR_GRAY2RGB); // 灰度复制成 3 个相同通道

    cv::Mat resized;
    cv::resize(rgb, resized, cv::Size(m_config.inputSize, m_config.inputSize)); // 默认 256×256

    resized.convertTo(resized, CV_32F, 1.0 / 255.0); // 当前预处理：像素除以 255，转换为 float32

    cv::Mat nchw;
    cv::dnn::blobFromImage(resized, nchw); // HWC → NCHW

    return nchw;
}

DetectionLevel DeepWaferDetector::classifyScore(float score) const
{
    if(score < m_config.goodThreshold)// 低于良品阈值
        return DetectionLevel::Good;
    if(score < m_config.ngThreshold)// 介于良品阈值和 NG 阈值之间
         return DetectionLevel::Warning;
        //(else) 达到或超过 NG 阈值
         return DetectionLevel::Ng;
}

/*
 * 单帧流程：检查图像 → 预处理 → 创建输入张量 → Run 推理 → 检查并读取输出 → 生成显示图。
 * loadModel() 检查的是模型“声明”的接口；这里检查这一帧“实际返回”的数据。
 * 失败时让异常交给 WaferAlgorithm::processPendingFrames()，由它停止并通知主窗口。
 * 只有完整处理成功才 return result，避免把默认分数或半成品当成正常检测结果。
 */
AlgoResult DeepWaferDetector::processFrame(const cv::Mat& frame,qint64 frameIdx, qint64 timestampMs)
{
    //创建结果容器
    AlgoResult result;
    result.frameIdx    = frameIdx;
    result.timestampMs = timestampMs;
    result.mode        = DetectionMode::DeepLearning;
    result.fi = result.p95 = result.hotRatio = -1.0; // DL 模式下不计算传统指标，-1 表示无效

    // 1. 先检查前提条件，避免把空图像或不支持的格式交给 OpenCV/模型。
    if (!isLoaded())
        throw std::runtime_error("DL model not loaded");
    // CV_8UC1：8 位单通道灰度；CV_8UC3：8 位三通道 BGR。
    if (frame.empty() || (frame.type() != CV_8UC1 && frame.type() != CV_8UC3))
        throw std::runtime_error("DL input frame must be a non-empty 8-bit grayscale or BGR image");

    // 显示用灰度（叠加热力图时用）
    cv::Mat gray;
    if (frame.channels() == 3)
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    else
        gray = frame;

    // 喂给模型的输入：保留彩色（训练数据可能含颜色信息，灰度化会丢失特征）
    // 若帧本身是灰度图，preprocess 内部会自动转 3 通道
    cv::Mat modelInput = frame;

  auto t0 = std::chrono::steady_clock::now();// 计时：推理开始

  // ══════════════════════════════════════════════
    //ONNX Runtime 推理
  // ══════════════════════════════════════════════

    // 2. 预处理：BGR 原样或灰度复制 → resize → 像素 /255 → NCHW。
    cv::Mat nchw=preprocess(modelInput);

    // 3. 给图像数据附上形状，构造 ONNX Runtime 能识别的输入张量。
    // 此重载直接使用 nchw 的内存；同步 Run 返回前，nchw 必须一直有效。
    std::array<int64_t,4> intputshape{1,3,m_config.inputSize,m_config.inputSize};
    auto inputTensor = Ort::Value::CreateTensor<float>//手动创建张量容器Ort::Value
    (
        m_memoryInfo,               // 内存分配策略（CPU、连续）
         nchw.ptr<float>(),         // 数据指针
          nchw.total(),             // 元素总数（= 1×3×inputSize×inputSize）
           intputshape.data(),      // 形状 [1,3,inputSize,inputSize]
            intputshape.size());    // 形状维度数=4

    // 4. 同步推理：传入 input，只请求 pred_score 和 anomaly_map。
    // outputs 的顺序由 outputnames 决定：第 0 个是分数，第 1 个是热力图。
    const char* inputnames[] = {"input"}; // 输入节点名
    const char* outputnames[] = {"pred_score", "anomaly_map"}; // 输出节点名（与模型预设必须一致！）
    Ort::RunOptions run_options;  // 默认构造，创建有效配置（不能传 nullptr）
    auto outputs = m_session->Run( //创建一堆张量的集合std::vector<Ort::Value>
        run_options, inputnames, &inputTensor, 1, outputnames, 2);// 请求两个输出，模型本身可以有更多输出
    
  // ══════════════════════════════════════════════
     //拆包结果
  // ══════════════════════════════════════════════    
    
   // 5. 先检查结果数量和类型，再访问 outputs[0]/[1]。
   // IsTensor() 确认这是张量，而不是其他 ONNX 数据结构。
   if (outputs.size() != 2 || !outputs[0].IsTensor() || !outputs[1].IsTensor())
       throw std::runtime_error("ONNX inference did not return two tensors");
   // GetElementCount() 返回张量内的元素总数；单帧分数必须只有一个 float。
   auto scoreInfo = outputs[0].GetTensorTypeAndShapeInfo();
   if (scoreInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
       scoreInfo.GetElementCount() != 1)
       throw std::runtime_error("ONNX pred_score must contain one float32 value");
   // GetTensorData<float>() 取得只读数据指针；确认类型和数量后才能安全读取第一个数。
   float score = outputs[0].GetTensorData<float>()[0];//拆包分数
   // NaN 表示“非数值”，Inf 表示无穷大；它们不能参与正常的等级判断。
   if (!std::isfinite(score))
       throw std::runtime_error("ONNX pred_score is not finite");
   result.anomalyScore=score;
   result.level=classifyScore(score);//判定等级

  // 6. 检查实际热力图形状，再转成 OpenCV 图像。
   auto& heatmapTensor=outputs[1];
   auto heatmapInfo = heatmapTensor.GetTensorTypeAndShapeInfo();
   auto heatmapShape=heatmapInfo.GetShape();//返回形状[1,1,H,W]
   // 实际形状必须是 [1,1,H,W]，H/W 已确定且大于 0；元素个数应等于 H×W。
   // OpenCV 行列数使用 int，先检查上限再 static_cast<int>()，防止转换后出错。
   // 乘积先转为 size_t，避免两个 int 相乘时溢出；先检查 4 维再访问各维度。
   if (heatmapInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
       heatmapShape.size() != 4 || heatmapShape[0] != 1 || heatmapShape[1] != 1 ||
       heatmapShape[2] <= 0 || heatmapShape[3] <= 0 ||
       heatmapShape[2] > std::numeric_limits<int>::max() ||
       heatmapShape[3] > std::numeric_limits<int>::max() ||
       heatmapInfo.GetElementCount() !=
           static_cast<size_t>(heatmapShape[2]) * static_cast<size_t>(heatmapShape[3]))
       throw std::runtime_error("ONNX anomaly_map must be float32 [1,1,H,W]");
   int mapH=static_cast<int>(heatmapShape[2]);
   int mapW=static_cast<int>(heatmapShape[3]);
   cv::Mat rawheatmap(mapH,mapW,CV_32FC1); // 32 位浮点、单通道图像
   // total() 是像素总数，elemSize() 是每个像素的字节数，两者相乘得到复制字节数。
   // memcpy 深拷贝到 rawheatmap 自己的内存，outputs 释放后图像数据仍然有效。
   std::memcpy(
   rawheatmap.data, heatmapTensor.GetTensorData<float>(), rawheatmap.total() * rawheatmap.elemSize());//深拷贝数据到cv::Mat
   // 检查全部热力图像素，防止 NaN/Inf 继续进入归一化和显示流程。
   if (!cv::checkRange(rawheatmap))
       throw std::runtime_error("ONNX anomaly_map contains NaN or Inf");

  // 7. 为显示把热力图缩放到 0～255，再用颜色表示强弱；不会改动上面的异常分数。
    cv::Mat heat8u;
    cv::normalize(rawheatmap, heat8u, 0, 255, cv::NORM_MINMAX);
    heat8u.convertTo(heat8u, CV_8UC1);

    cv::Mat heatColor;
    cv::applyColorMap(heat8u, heatColor, cv::COLORMAP_JET);
    cv::resize(heatColor, heatColor, gray.size());

  // 8. 填充原图、彩色热力图、灰度原图与热力图叠加图。
    result.frameOriginal = frame.clone();           // 左上：原图
    result.frameSpectrum = heatColor.clone();       // 右上：热力图 
    cv::Mat originalColor;
    cv::cvtColor(gray, originalColor, cv::COLOR_GRAY2BGR);
    cv::addWeighted(originalColor,0.6,   // 第一张：原图，权重 60%
                    heatColor,    0.4,   // 第二张：热力图，权重 40%
                    0,                      // 亮度偏移，一般写 0
                    result.frameFlatness);  // 输出到第三个视图
    
   auto t1 = std::chrono::steady_clock::now();// 计时：包含预处理、推理和后处理的耗时
   result.inferenceTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
   // 帧日志由 WaferAlgorithm::processPendingFrames() 统一打印，避免每帧重复

   // 任一步抛出异常都会提前离开函数；只有成功执行完上述步骤，调用者才得到结果。
   return result;

}
