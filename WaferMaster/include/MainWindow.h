#pragma once

#include <QMainWindow>
#include <QThread>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QPoint>
#include <opencv2/opencv.hpp>
#include "Common.h"
#include "ResultLogger.h"

class QLabel;
class FrameProducer;
class WaferAlgorithm;
class RoiViewerDialog;
class CommunicationManager;

namespace Ui {
class MainWindow;// 在 Ui 命名空间中前向声明 MainWindow 类
}

/**  @brief 表现层：主窗口，负责线程编排、显示刷新、状态管理、观察 ROI 交互
          不负责采集和算法，不写 CSV

## MainWindow 工作流程

###  主链路
 1. 构造 → setupUiState() 初始化 UI 控件初始状态
 2. setupWorkers() 创建两条空 QThread（Worker 对象在 onStartClicked() 全量重建）
 3. setupConnections() 建立信号槽连接（采集→算法→显示）
 4. 用户点击"开始检测" → onStartClicked() → 注入配置 → 启动线程
 5. 算法结果到达 → onAlgorithmResultReady() → refreshMainViews()→ updateStatusBarText()                              
 6. 用户点击"停止检测" → onStopClicked() → 停止线程 → cleanupWorkers()
###  观察 ROI
 7. eventFilter() 在原图 QLabel 上捕获鼠标框选交互
 8. imageDisplayRect() 将 QLabel 像素坐标映射到实际图像像素坐标
 9. refreshObserveRoiViews() 裁出 ROI 原图 + ROI 平坦图 → 推送到 RoiViewerDialog 弹窗
 10. 观察 ROI 仅在 Idle / Stopped 状态可用，Running 时自动禁用

### 线程安全
 - 主线程操作所有 QWidget / QLabel / QStatusBar
 - FrameProducer 和 WaferAlgorithm 通过 moveToThread() 迁移到独立线程
 - 跨线程 cv::Mat 已由算法层 clone() 确保独立所有权
*/
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /// @brief 构造主窗口，初始化 UI 并设置控件初始状态
    /// @param parent 父窗口指针（默认无父窗口）
    explicit MainWindow(QWidget* parent = nullptr);

    /// @brief 析构，确保线程安全退出并释放资源
    ~MainWindow();

protected:
    /// @brief 事件过滤器：实现原图 QLabel 上的鼠标框选交互
    ///        捕获 mousePress / mouseMove / mouseRelease 事件，实时更新观察 ROI
    /// @param watched 被监视的 QObject（QWidget）
    /// @param event   待处理的事件
    /// @return true=事件已被拦截，不再继续传递
    bool eventFilter(QObject* watched, QEvent* event) override;

    /// @brief 窗口尺寸变化时重新缩放所有图像显示，确保全屏/恢复后比例正确
    /// @param event 窗口 resize 事件
    void resizeEvent(QResizeEvent* event) override;

private slots:
    // ========================================================================
    // UI 交互槽函数
    // ========================================================================

    /// @brief 浏览按钮：根据输入源类型弹出文件/文件夹选择对话框
    ///        AVI 模式 → QFileDialog 选 .avi 文件；图片序列模式 → 选文件夹
    void onBrowseClicked();

    /// @brief 开始检测按钮：从 UI 控件收集参数 → buildSourceConfig()
    ///        → setupWorkers() 创建线程和工作对象 → inject 配置 → 启动线程
    void onStartClicked();

    /// @brief 停止检测按钮：向 FrameProducer 和 WaferAlgorithm 发送 stop()
    ///        → 等待线程结束 → cleanupWorkers() 回收资源 → 恢复 UI 状态
    void onStopClicked();

    /// @brief 观察 ROI 开关：用户勾选/取消勾选时切换 m_observeRoiEnabled
    ///        仅在 Idle / Stopped 状态下允许操作，Running 时自动禁用
    /// @param checked 是否启用观察 ROI
    void onObserveRoiToggled(bool checked);

    /// @brief 输入源类型切换（cmbSourceType）：图片序列 ↔ AVI视频
    ///        切换时更新 editSourcePath 的 placeholder 提示文本
    /// @param index comboBox 当前选中项索引（0=图片序列, 1=AVI视频）
    void onSourceTypeChanged(int index);

    /// @brief 亮度滑块变化 → 重新以亮度/对比度参数刷新所有显示图
    ///        仅影响显示渲染，不写入算法结果
    /// @param value 亮度值（0-100）
    void onBrightnessChanged(int value);

    /// @brief 对比度滑块变化 → 重新以亮度/对比度参数刷新所有显示图
    ///        仅影响显示渲染，不写入算法结果
    /// @param value 对比度值（0-100）
    void onContrastChanged(int value);

    // ========================================================================
    // 跨线程信号接收槽
    // ========================================================================

    /// @brief 接收采集层发出的源信息（路径 + 分辨率），更新状态栏显示
    /// @param path       输入源路径（AVI 文件 或 图片文件夹）
    /// @param resolution 视频/首帧图像分辨率（宽×高）
    void onSourceInfoReady(const QString& path, const QSize& resolution);

    /// @brief 接收算法层发出的单帧检测结果，刷新三张主图 + 状态栏 + 观察 ROI
    /// @param result 包含 fi/p95/hotRatio/level 和三张显示用 cv::Mat
    void onAlgorithmResultReady(const AlgoResult& result);

    /// @brief 接收采集层或算法层发出的错误消息，通过 QMessageBox 显示
    /// @param message 可读的错误描述文本
    void onWorkerError(const QString& message);

    /// @brief 采集线程 finished 信号 → 更新运行状态，检查是否两个线程都已完成
    void onProducerFinished();

    /// @brief 算法线程 finished 信号 → 更新运行状态，检查是否两个线程都已完成
    void onAlgorithmFinished();

    /// @brief 接收 QtLogBridge 信号 → 追加到 plainTextEditLog
    void onLogMessage(const QString& message);

    // ========================================================================
    // TCP通信层槽函数 
    // ========================================================================

    /// @brief TCP 客户端发送 START → 等价执行 onStartClicked()
    void onCommStartRequested();

    /// @brief TCP 客户端发送 STOP → 等价执行 onStopClicked()
    void onCommStopRequested();

    // ========================================================================
    // 查询历史记录
    // ========================================================================

    /// @brief 查询历史记录：弹出等级选择框 → SQLite查询 → 结果表格对话框
    void onQueryHistoryClicked();

private:
    // ========================================================================
    // 初始化和清理
    // ========================================================================

    /// @brief 设置 UI 控件初始状态：按钮启用/禁用、状态栏默认文本、观察 ROI 初始关闭
    void setupUiState();

    /// @brief 创建工作线程和工作对象（无父对象构造 → moveToThread）
    ///        1. new FrameProducer / WaferAlgorithm（无 parent）
    ///        2. new QThread
    ///        3. moveToThread()
    ///        4. connect 线程 start/finished 信号
    void setupWorkers();

    /// @brief 建立跨线程信号槽连接：采集→算法→主窗口
    ///        包括 frameAvailable → processPendingFrames
    ///           resultReady → onAlgorithmResultReady
    ///           errorOccurred → onWorkerError
    ///           finished → onProducerFinished / onAlgorithmFinished
    void setupConnections();

    /// @brief 初始化 TCP 通信层：创建 CommunicationManager、连接信号槽、启动服务
    void setupCommunication();

    /// @brief 初始化或重新配置结果日志记录器（CSV + SQLite 双写）
    ///        csvPath / dbPath 为空时自动推导至 exe 所在目录的 logs/ 子目录
    /// @param mode    存储模式：None / Csv / Sqlite / Both
    /// @param csvPath CSV 文件路径（可选，空则自动推导）
    /// @param dbPath  SQLite 数据库路径（可选，空则自动推导）
    void initResultLogger(StorageMode mode,
                          const QString& csvPath = QString(),
                          const QString& dbPath = QString());

    /// @brief 安全停止线程并释放资源（quit → wait → deleteLater）
    ///        先停 FrameProducer 再停 WaferAlgorithm，确保不再有新帧入队
    void cleanupWorkers();

    // ========================================================================
    // 配置与状态
    // ========================================================================

    /// @brief 从 UI 控件收集当前参数，构造 SourceConfig
    ///        输入源类型来自 cmbSourceType，路径来自 editSourcePath，帧间隔暂用默认值
    /// @return 填充完整的 SourceConfig
    SourceConfig buildSourceConfig() const;

    /// @brief 更新运行状态，并同步调整 UI 控件启用/禁用
    ///        Idle→启用开始/浏览；Running→启用停止，禁用开始；Stopped→恢复开始/浏览
    /// @param state 新的运行状态
    void updateRunState(RunState state);

    /// @brief 更新状态栏文本：路径、分辨率、帧号、FI、P95、HotRatio、Level、运行状态
    void updateStatusBarText();
    
    /// @brief 刷新参数显示标签：lblAlgoRoi / lblFiThresh / lblHotRatioThresh / lblObserveRoiInfo
    ///        在 onAlgorithmResultReady() 和 slider 变化时调用
    void updateParamLabels();
    // ========================================================================
    // 显示刷新（2）
    // ========================================================================

    /// @brief 刷新三张主图显示：原图、频谱图、平坦图
    ///        调用 showMatOnLabel() 将 cv::Mat 转 QImage 后设置到对应 QLabel
    /// @param result 算法层输出的完整检测结果
    void refreshMainViews(const AlgoResult& result);

    /// @brief 刷新焦点 ROI 小图：ROI 原图 + ROI 平坦图
    ///        仅在观察 ROI 已启用（m_observeRoiEnabled）且框选有效时执行
    ///        从 m_lastResult.frameOriginal / frameFlatness 按 m_observeRoiRect 裁出
    void refreshObserveRoiViews();

    // ========================================================================
    // 图像工具函数
    // ========================================================================

    /// @brief 将 OpenCV cv::Mat 转换为 Qt QImage（BGR → RGB 色彩通道转换）
    /// @param mat 输入 OpenCV 图像（支持 CV_8UC1 / CV_8UC3）
    /// @return Qt 可显示的 QImage，若输入为空则返回空 QImage
    QImage cvMatToQImage(const cv::Mat& mat) const;

    /// @brief 将 cv::Mat 显示到指定 QLabel 上（cvMatToQImage + setPixmap 的便捷封装）
    ///        自动处理空图像和缩放显示
    /// @param label 目标 QLabel 控件指针
    /// @param mat   要显示的 OpenCV 图像
    void showMatOnLabel(QLabel* label, const cv::Mat& mat) const;

    // ========================================================================
    // 观察 ROI 工具函数
    // ========================================================================

    /// @brief 在原图 QLabel 上绘制观察 ROI 红色虚线框
    ///        在 eventFilter() 的 Paint 事件中调用
    void drawRoiRect();

    /// @brief 计算 QLabel 内 pixmap 的实际显示区域（KeepAspectRatio 居中后的矩形）
    ///        用于 Paint 分支绘制底图和鼠标坐标反算时的留白 offset 扣除
    /// @param label 目标 QLabel 指针
    /// @return pixmap 在 label 内的居中矩形，若 label 为空或 pixmap 为空则返回空 QRect
    QRect imageDisplayRect(QLabel* label) const;

    // ========================================================================
    // 成员变量 — UI
    // ========================================================================
    Ui::MainWindow* ui = nullptr;   // Qt Designer 自动生成的 UI 类指针

    // ========================================================================
    // 成员变量 — 线程与工作对象
    // ========================================================================
    QThread*        m_producerThread   = nullptr; // 采集线程
    QThread*        m_algorithmThread  = nullptr; // 算法线程
    FrameProducer*  m_producer         = nullptr; // 采集层工作对象（无父对象）
    WaferAlgorithm* m_algorithm        = nullptr; // 算法层工作对象（无父对象）

    // ========================================================================
    // 成员变量 — 系统状态
    // ========================================================================
    RunState        m_runState    = RunState::Idle; // 当前运行状态
    SourceConfig    m_sourceConfig;                  // 当前输入源配置（从 UI 收集）
    AlgoResult      m_lastResult;                    // 最后一帧的算法结果（用于观察 ROI 裁切和状态栏刷新）
    AlgoConfig      m_currentAlgoCfg;                // 当前算法配置（阈值，供左侧参数面板显示）

    // ========================================================================
    // 成员变量 — 显示状态
    // ========================================================================
    QString         m_currentPath;       // 当前输入源路径（状态栏显示用）
    QSize           m_currentResolution; // 当前输入源分辨率（状态栏显示用）
    int             m_brightness = 0;    // 亮度调整值（sliderBright 当前值）
    int             m_contrast   = 0;    // 对比度调整值（sliderContrast 当前值）

    // ========================================================================
    // 成员变量 — 观察 ROI
    // ========================================================================
    bool   m_observeRoiEnabled     = false; // 观察 ROI 功能开关（由 CheckBox 控制）
    bool   m_isSelectingObserveRoi = false; // 是否正在框选观察 ROI（鼠标按下后未释放）
    QPoint m_observeStartPoint;            // 观察 ROI 框选起点（QLabel 像素坐标）
    QPoint m_observeEndPoint;              // 观察 ROI 框选终点（QLabel 像素坐标）
    QRect  m_observeRoiRect;              // 最终的观察 ROI 矩形（经 normalize + clamp 处理）

    // ========================================================================
    // 成员变量 — ROI 弹窗
    // ========================================================================
    RoiViewerDialog* m_roiDialog = nullptr; // 观察 ROI 独立弹窗（框选完成后弹出）

    // ========================================================================
    // 成员变量 — 通信层
    // ========================================================================
    CommunicationManager* m_comm = nullptr; // TCP 通信管理对象

    // ========================================================================
    // 成员变量 — 结果记录
    // ========================================================================
    ResultLogger* m_resultLogger = nullptr;  // 检测结果存储器
};
