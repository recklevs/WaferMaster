#include "MainWindow.h"
#include "ui_WaferMaster.h"
#include "FrameProducer.h"
#include "WaferAlgorithm.h"
#include "RoiViewerDialog.h"
#include "CommunicationManager.h"
#include "Logger.h"

#include <QFileDialog>
#include <QHostAddress>
#include <QMessageBox>
#include <QPixmap>
#include <QLabel>
#include <QStatusBar>
#include <QPainter>
#include <QMouseEvent>

// ============================================================================
// 构造 & 析构
// ============================================================================

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)//创建一个 Ui::MainWindow 对象，并将其地址赋值给成员指针 ui。
{
    ui->setupUi(this);//调用 ui 对象的 setupUi() 方法，传入 this 指针（MainWindow 实例），完成 UI 初始化。
    setupUiState();//设置控件初始值
    setupWorkers();//创建线程对象
    setupConnections();//建立信号槽连接
    setupCommunication();//创建并启动 TCP 通信服务器
}

MainWindow::~MainWindow()
{
    cleanupWorkers();
    if (m_comm) {
        m_comm->stopServer();
    }
    delete m_roiDialog;//防御性删除，确保即使未创建也安全
    delete ui;
}

// ============================================================================
// 初始化和清理
// ============================================================================

void MainWindow::setupUiState()
{
    // 按钮初始状态
    ui->btnStart->setEnabled(true);
    ui->btnStop->setEnabled(false);
    ui->btnBrowse->setEnabled(true);

    // 观察 ROI 按钮初始未勾选
    ui->btnObserveRoi->setChecked(false);

    // 滑块初始值（范围需与 .ui 中定义一致，默认 0~100）
    ui->sliderBright->setValue(0);
    ui->sliderContrast->setValue(0);

    // 参数标签初始显示 "-"
    ui->lblAlgoRoi->setText(QStringLiteral("算法ROI: -"));
    ui->lblFiThresh->setText(QStringLiteral("FI阈值: -"));
    ui->lblHotRatioThresh->setText(QStringLiteral("HotRatio阈值: -"));
    ui->lblObserveRoiInfo->setText(QStringLiteral("观察ROI: -"));

    // 为原图 QLabel 安装事件过滤器，用于观察 ROI 框选交互
    ui->lblViewOriginal->installEventFilter(this);
    ui->lblViewOriginal->setMouseTracking(true);
    ui->lblViewOriginal->setCursor(Qt::ArrowCursor);

    // 状态栏初始文本
    if (ui->statusBar)
        ui->statusBar->showMessage(QStringLiteral("就绪"));

    // 输入源默认 placeholder（cmbSourceType 初始 index=0 即图片序列）
    ui->editSourcePath->setPlaceholderText(QStringLiteral("请选择图片序列文件夹"));
}

void MainWindow::setupWorkers()
{
    // 只创建线程对象（由 parent=this 管理生命周期）
    // Worker 对象由 onStartClicked() 全量重建
// 创建两条独立线程    
    m_producerThread  = new QThread(this);
    m_algorithmThread = new QThread(this);
}

void MainWindow::setupConnections()
{
    // ========================================================================
    // UI 控件 → MainWindow 槽（生命周期与 MainWindow 等长，此处一次性连接）
    // ========================================================================

    // 手动 connect，使用自定义槽名
    connect(ui->btnBrowse,      &QPushButton::clicked, this, &MainWindow::onBrowseClicked);
    connect(ui->btnStart,       &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui->btnStop,        &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(ui->btnObserveRoi,  &QPushButton::toggled, this, &MainWindow::onObserveRoiToggled);

    connect(ui->cmbSourceType, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSourceTypeChanged);

    connect(ui->sliderBright,   &QSlider::valueChanged, this, &MainWindow::onBrightnessChanged);
    connect(ui->sliderContrast, &QSlider::valueChanged, this, &MainWindow::onContrastChanged);

    // QtLogBridge → 日志控件（Logger::init() 已创建 QtLogBridge）
    QtLogBridge* bridge = Logger::getBridge();
    if (bridge)
    {
        connect(bridge, &QtLogBridge::logMessage, this, [this](const QString& msg) {
            ui->plainTextEditLog->appendPlainText(msg);
        });
    }

    // 跨线程 Worker 连接由 onStartClicked() 全量重建，此处不连接
}

void MainWindow::setupCommunication()
{
    // 创建并启动 TCP 通信服务器
    m_comm = new CommunicationManager(this);
    connect(m_comm, &CommunicationManager::startRequested,
            this, &MainWindow::onCommStartRequested);
    connect(m_comm, &CommunicationManager::stopRequested,
            this, &MainWindow::onCommStopRequested);
    connect(m_comm, &CommunicationManager::listeningStateChanged,
            this, [this](bool listening, const QString& msg) {
        ui->lblCommListenState->setText(
            QStringLiteral("状态: %1").arg(listening
                ? QStringLiteral("监听中") : msg));
        ui->lblCommMode->setText(QStringLiteral("模式: TCP Server"));
        ui->lblCommAddress->setText(QStringLiteral("地址: 127.0.0.1"));
        ui->lblCommPort->setText(QStringLiteral("端口: 9000"));
    });
    connect(m_comm, &CommunicationManager::clientStateChanged,
            this, [this](bool connected, int count) {
        ui->lblCommClientState->setText(
            QStringLiteral("客户端: %1").arg(connected
                ? QStringLiteral("%1个已连接").arg(count)
                : QStringLiteral("未连接")));
    });
    connect(m_comm, &CommunicationManager::logMessage,
            this, &MainWindow::onLogMessage);
    m_comm->startServer(QHostAddress::LocalHost, 9000);
}

void MainWindow::cleanupWorkers()
{
    // 先停采集再停算法，确保不再有新帧入队
    if (m_producer)
    {
        //通过 Qt 的元对象系统（QMetaObject）调用 m_producer 的 stop() 方法，使用排队执行确保跨线程调用安全
        QMetaObject::invokeMethod(m_producer, &FrameProducer::stop, Qt::QueuedConnection);
    }
    if (m_algorithm)
    {
        QMetaObject::invokeMethod(m_algorithm, &WaferAlgorithm::stop, Qt::QueuedConnection);
    }

    // 等待线程退出（quit() 会处理完已排队的 invokeMethod 调用再退出）
    // QTimer 驱动版 stop() 立即中断，wait(500) 绰绰有余
    if (m_producerThread && m_producerThread->isRunning())//如果线程存在且正在运行
    {
        m_producerThread->quit();
        m_producerThread->wait(500);
    }
    if (m_algorithmThread && m_algorithmThread->isRunning())
    {
        m_algorithmThread->quit();
        m_algorithmThread->wait(500);
    }
/*
为什么 QThread 用 `deleteLater` 而 Worker 用 `delete`：
退出2个子线程后，m_producer和 m_algorithm 已经不再运行了，但它们的线程对象 m_producerThread 和 m_algorithmThread 仍然存在（归属主线程）
主线程的事件循环还在运行。deleteLater()把删除指令放入主线程的队列，等事件循环空闲时再执行删除，确保线程对象在安全的时机被销毁。
*/
    delete m_producer;
    m_producer = nullptr;
    delete m_algorithm;
    m_algorithm = nullptr;

    // 线程通过 deleteLater 延迟回收，让 Qt 在事件循环中安全销毁 QThread 对象
    if (m_producerThread)
    {
        m_producerThread->deleteLater();
        m_producerThread = nullptr;
    }
    if (m_algorithmThread)
    {
        m_algorithmThread->deleteLater();
        m_algorithmThread = nullptr;
    }
}

// ============================================================================
// UI 交互槽
// ============================================================================

void MainWindow::onBrowseClicked()
{
    const int idx = ui->cmbSourceType->currentIndex();//获取索引编号0/1

    if (idx == 0) // 图片序列 → 选文件夹
    {
        const QString dir = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("选择图片序列文件夹"));
        if (!dir.isEmpty())
            ui->editSourcePath->setText(dir);
    }
    else // AVI 视频 → 选 .avi 文件
    {
        const QString file = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("选择AVI视频文件"),
            QString(),
            QStringLiteral("AVI 文件 (*.avi)"));
        if (!file.isEmpty())
            ui->editSourcePath->setText(file);
    }
}
/*核心启动检测
onStartClicked()
  │
  ├─ buildSourceConfig()        从 UI 收参
  ├─ cleanupWorkers()           清旧资源
  ├─ new QThread × 2            建新线程
  ├─ new Worker × 2             建工作对象
  ├─ moveToThread × 2           搬家
  ├─ connect × 9                建跨线程信号链
  ├─ setSourceConfig()          注入配置
  ├─ setFrameProducer()         算法绑定生产者
  ├─ thread->start() × 2        启动线程
  └─ updateRunState(Running)    状态机切运行
*/
void MainWindow::onStartClicked()
{
    // 从 UI 收参
    m_sourceConfig = buildSourceConfig();

    // 路径非空校验
    if (m_sourceConfig.sourcePath.isEmpty())
    {
        QMessageBox::warning(this,
            QStringLiteral("参数错误"),
            QStringLiteral("请先选择输入源路径。"));
        return;
    }

    // 全量重建 — 每次启动都重建线程和工作对象，状态最清晰
    // 先确保旧资源已释放
    cleanupWorkers();

    // 重建线程
    if (!m_producerThread)
        m_producerThread = new QThread(this);
    if (!m_algorithmThread)
        m_algorithmThread = new QThread(this);

    // 先创建 Algorithm，再创建 Producer
    m_algorithm = new WaferAlgorithm();
    m_producer = new FrameProducer();

    // 迁移到各自线程
    m_producer->moveToThread(m_producerThread);
    m_algorithm->moveToThread(m_algorithmThread);

    // ====================================================================
    // 重建全部跨线程连接（每次全量重建，简洁无残留）
    // ====================================================================

    // FrameProducer → WaferAlgorithm 唤醒
    connect(m_producer,  &FrameProducer::frameAvailable,
            m_algorithm, &WaferAlgorithm::processPendingFrames);

    // FrameProducer → MainWindow
    connect(m_producer, &FrameProducer::sourceInfoReady,
            this,       &MainWindow::onSourceInfoReady);
    connect(m_producer, &FrameProducer::errorOccurred,
            this,       &MainWindow::onWorkerError);
    connect(m_producer, &FrameProducer::finished,
            this,       &MainWindow::onProducerFinished);

    // WaferAlgorithm → MainWindow
    connect(m_algorithm, &WaferAlgorithm::resultReady,
            this,        &MainWindow::onAlgorithmResultReady);
    connect(m_algorithm, &WaferAlgorithm::errorOccurred,
            this,        &MainWindow::onWorkerError);
    connect(m_algorithm, &WaferAlgorithm::finished,
            this,        &MainWindow::onAlgorithmFinished);

    // 线程 started → Worker start 槽
    connect(m_producerThread,  &QThread::started,
            m_producer,        &FrameProducer::start);
    connect(m_algorithmThread, &QThread::started,
            m_algorithm,       &WaferAlgorithm::start);

    // 注入配置
    m_producer->setSourceConfig(m_sourceConfig);//生产者注入输入源配置
    m_currentAlgoCfg = AlgoConfig{}; // 使用默认参数（Common.h 中的阈值）
    m_algorithm->setAlgoConfig(m_currentAlgoCfg);//算法处理器注入算法参数配置
    m_algorithm->setFrameProducer(m_producer);//算法处理器注入生产者指针，供拉帧使用

    // 启动线程
    m_producerThread->start();
    m_algorithmThread->start();

    updateRunState(RunState::Running);
}

void MainWindow::onStopClicked()
{
    cleanupWorkers();
    updateRunState(RunState::Stopped);
}

void MainWindow::onObserveRoiToggled(bool checked)
{
    // Running 时禁止切换观察 ROI，恢复按钮状态
    if (m_runState == RunState::Running)
    {
        ui->btnObserveRoi->blockSignals(true);// 暂时阻断信号
        ui->btnObserveRoi->setChecked(m_observeRoiEnabled);// 恢复按钮状态
        ui->btnObserveRoi->blockSignals(false);// 重新连接信号
        return;
    }

    m_observeRoiEnabled = checked;

    // 若开启且有缓存的最后一帧结果，立即刷新 ROI 小图
    if (checked)
    {
        ui->lblViewOriginal->setCursor(Qt::CrossCursor);
        if (!m_lastResult.frameOriginal.empty())
            refreshObserveRoiViews();
    }
    else
    {
        // 关闭观察 ROI：还原光标、清除原图上红框、关闭弹窗
        ui->lblViewOriginal->setCursor(Qt::ArrowCursor);
        if (m_roiDialog)
            m_roiDialog->close();
        if (!m_lastResult.frameOriginal.empty())
            showMatOnLabel(ui->lblViewOriginal, m_lastResult.frameOriginal);
    }

    updateParamLabels();
}

void MainWindow::onSourceTypeChanged(int index)
{
    if (index == 0)
    {
        // 图片序列
        ui->editSourcePath->setPlaceholderText(QStringLiteral("请选择图片序列文件夹"));
    }
    else
    {
        // AVI 视频
        ui->editSourcePath->setPlaceholderText(QStringLiteral("请选择AVI视频文件"));
    }
}

void MainWindow::onBrightnessChanged(int value)
{
    m_brightness = value;

    // 有缓存结果时重新刷新显示（仅影响渲染，不改变 m_lastResult 数值）
    if (!m_lastResult.frameOriginal.empty())
    {
        refreshMainViews(m_lastResult);
        if (m_observeRoiEnabled)
            refreshObserveRoiViews();
    }
}

void MainWindow::onContrastChanged(int value)
{
    m_contrast = value;

    if (!m_lastResult.frameOriginal.empty())
    {
        refreshMainViews(m_lastResult);
        if (m_observeRoiEnabled)
            refreshObserveRoiViews();
    }
}

// ============================================================================
// 跨线程信号接收槽
// ============================================================================

void MainWindow::onSourceInfoReady(const QString& path, const QSize& resolution)
{
    m_currentPath       = path;
    m_currentResolution = resolution;
    updateStatusBarText();
}

void MainWindow::onAlgorithmResultReady(const AlgoResult& result)
{
    m_lastResult = result;  // 缓存最后一帧，供 slider 刷新和观察 ROI 使用

    refreshMainViews(result);
    updateStatusBarText();
    updateParamLabels();

    // 若观察 ROI 已启用且框选有效，刷新 ROI 小图
    if (m_observeRoiEnabled && !m_observeRoiRect.isNull())
        refreshObserveRoiViews();

    // 通知通信层最新结果
    if (m_comm)
        m_comm->updateLatestStatus(m_runState, result);
}//Algorithm每处理完一帧后，都会调用这个槽函数来更新界面显示和状态栏信息。

void MainWindow::onWorkerError(const QString& message)
{
    QMessageBox::critical(this, QStringLiteral("错误"), message);
}

void MainWindow::onProducerFinished()
{
    // 空函数。finished 信号已连接到 thread->quit()，线程退出由 cleanupWorkers 统一管理
}

void MainWindow::onAlgorithmFinished()
{
    // 算法线程结束，不做额外操作
}

void MainWindow::onLogMessage(const QString& message)
{
    if (ui->plainTextEditLog)
        ui->plainTextEditLog->appendPlainText(message);
}

// ============================================================================
// 通信层槽函数
// ============================================================================

void MainWindow::onCommStartRequested()
{
    // 仅在 Idle 或 Stopped 状态执行，避免重复启动
    if (m_runState == RunState::Idle || m_runState == RunState::Stopped)
    {
        onStartClicked();
    }
}

void MainWindow::onCommStopRequested()
{
    // 仅在 Running 状态执行
    if (m_runState == RunState::Running)
    {
        onStopClicked();
    }
}

// ============================================================================
// 配置与状态
// ============================================================================

SourceConfig MainWindow::buildSourceConfig() const
{
    SourceConfig cfg;

    const int idx = ui->cmbSourceType->currentIndex();
    cfg.sourceType = (idx == 0) ? InputSourceType::ImageSequence//三元运算符：idx==0 → ImageSequence
                          : InputSourceType::AviVideo;//idx!=0 → AviVideo
    cfg.sourcePath = ui->editSourcePath->text().trimmed();//获取输入路径，trimmed()去除首尾空白
    return cfg;
}

void MainWindow::updateRunState(RunState state)
{
    m_runState = state;

    switch (state)
    {
    case RunState::Idle:
    case RunState::Stopped:
        ui->btnStart->setEnabled(true);
        ui->btnStop->setEnabled(false);
        ui->btnBrowse->setEnabled(true);
        break;
    case RunState::Running:
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(true);
        ui->btnBrowse->setEnabled(false);
        // 运行中强制关闭观察 ROI
        if (m_observeRoiEnabled)
        {
            m_observeRoiEnabled = false;
            ui->btnObserveRoi->blockSignals(true);
            ui->btnObserveRoi->setChecked(false);
            ui->btnObserveRoi->blockSignals(false);
            if (m_roiDialog)
                m_roiDialog->close();
            if (!m_lastResult.frameOriginal.empty())
                showMatOnLabel(ui->lblViewOriginal, m_lastResult.frameOriginal);
        }
        break;
    case RunState::Error:
        ui->btnStart->setEnabled(true);
        ui->btnStop->setEnabled(false);
        ui->btnBrowse->setEnabled(true);
        break;
    }

    // 通知通信层状态变更
    if (m_comm)
        m_comm->updateLatestStatus(state, m_lastResult);

    updateStatusBarText();
}

void MainWindow::updateStatusBarText()
{
    if (!ui->statusBar)//防御：如果.ui文件里没放 QStatusBar，直接返回
        return;

    QString text;

    // 路径
    if (!m_currentPath.isEmpty())
        text += QStringLiteral("源: ") + m_currentPath;

    // 分辨率
    if (m_currentResolution.isValid())
    {
        if (!text.isEmpty()) text += QStringLiteral(" | ");
        text += QStringLiteral("%1x%2")
                    .arg(m_currentResolution.width())
                    .arg(m_currentResolution.height());
    }

    // 帧号
    if (m_lastResult.frameIdx >= 0)
    {
        if (!text.isEmpty()) text += QStringLiteral(" | ");
        text += QStringLiteral("帧: %1").arg(m_lastResult.frameIdx);
    }

    // FI
    if (!text.isEmpty()) text += QStringLiteral(" | ");
    text += QStringLiteral("FI: %1").arg(m_lastResult.fi, 0, 'f', 2);

    // P95
    if (!text.isEmpty()) text += QStringLiteral(" | ");
    text += QStringLiteral("P95: %1").arg(m_lastResult.p95, 0, 'f', 1);

    // HotRatio
    if (!text.isEmpty()) text += QStringLiteral(" | ");
    text += QStringLiteral("Hot: %1%").arg(m_lastResult.hotRatio, 0, 'f', 1);

    // Level
    if (!text.isEmpty()) text += QStringLiteral(" | ");
    text += detectionLevelToString(m_lastResult.level);

    // 运行状态
    switch (m_runState)
    {
    case RunState::Idle:    text += QStringLiteral(" [就绪]"); break;
    case RunState::Running: text += QStringLiteral(" [运行中]"); break;
    case RunState::Stopped: text += QStringLiteral(" [已停止]"); break;
    case RunState::Error:   text += QStringLiteral(" [错误]"); break;
    }

    ui->statusBar->showMessage(text);
}

void MainWindow::updateParamLabels()
{
    // 算法 ROI 矩形
    if (m_lastResult.algoRoiRect.isValid())
    {
        ui->lblAlgoRoi->setText(
            QStringLiteral("算法ROI: (%1,%2) %3x%4")
                .arg(m_lastResult.algoRoiRect.x())
                .arg(m_lastResult.algoRoiRect.y())
                .arg(m_lastResult.algoRoiRect.width())
                .arg(m_lastResult.algoRoiRect.height()));
    }
    else
    {
        ui->lblAlgoRoi->setText(QStringLiteral("算法ROI: -"));
    }

    // FI 阈值
     ui->lblFiThresh->setText(
        QStringLiteral("FI阈值: %1").arg(m_currentAlgoCfg.fiThreshold, 0, 'f', 1));
    // HotRatio 阈值
     ui->lblHotRatioThresh->setText(
        QStringLiteral("HotRatio阈值: %1%").arg(m_currentAlgoCfg.hotRatioThreshold, 0, 'f', 1));
    // P95 阈值（来自 AlgoConfig，静态配置值）
    ui->lblP95Thresh->setText(
        QStringLiteral("P95阈值: %1").arg(m_currentAlgoCfg.p95Threshold, 0, 'f', 1));
    // 观察 ROI 信息
    if (m_observeRoiEnabled && !m_observeRoiRect.isNull())
    {
        ui->lblObserveRoiInfo->setText(
            QStringLiteral("观察ROI: (%1,%2) %3x%4")
                .arg(m_observeRoiRect.x())
                .arg(m_observeRoiRect.y())
                .arg(m_observeRoiRect.width())
                .arg(m_observeRoiRect.height()));
    }
    else
    {
        ui->lblObserveRoiInfo->setText(QStringLiteral("观察ROI: -"));
    }
}

// ============================================================================
// 显示刷新
// ============================================================================

void MainWindow::refreshMainViews(const AlgoResult& result)
{
    showMatOnLabel(ui->lblViewOriginal,  result.frameOriginal);
    showMatOnLabel(ui->lblViewSpectrum,  result.frameSpectrum);
    showMatOnLabel(ui->lblViewFlatness,  result.frameFlatness);
}

void MainWindow::refreshObserveRoiViews()
{
    if (!m_observeRoiEnabled || m_observeRoiRect.isNull())
        return;//// 观察 ROI 没开，或者还没框选，什么都不做

    // 从 m_lastResult 中裁切
    const QRect& r = m_observeRoiRect;

    // 推送到独立弹窗（若存在）
    if (m_roiDialog)
    {
        QImage imgOrig, imgFlat;
        if (!m_lastResult.frameOriginal.empty())
        {
            cv::Rect roiOrig(r.x(), r.y(), r.width(), r.height());//QRect → cv::Rect 转换
            roiOrig &= cv::Rect(0, 0, m_lastResult.frameOriginal.cols, m_lastResult.frameOriginal.rows);//求两个矩形交集
            if (roiOrig.width > 0 && roiOrig.height > 0)
                imgOrig = cvMatToQImage(m_lastResult.frameOriginal(roiOrig));
        }
        if (!m_lastResult.frameFlatness.empty())
        {
            cv::Rect roiFlat(r.x(), r.y(), r.width(), r.height());
            roiFlat &= cv::Rect(0, 0, m_lastResult.frameFlatness.cols, m_lastResult.frameFlatness.rows);
            if (roiFlat.width > 0 && roiFlat.height > 0)
                imgFlat = cvMatToQImage(m_lastResult.frameFlatness(roiFlat));
        }
        m_roiDialog->setImages(imgOrig, imgFlat);
    }
}

// ============================================================================
// 图像工具函数
// ============================================================================

QImage MainWindow::cvMatToQImage(const cv::Mat& mat) const
{
    if (mat.empty())
        return QImage();

    QImage img;

    switch (mat.type())
    {
    case CV_8UC3:
        // OpenCV 默认 BGR 排列，QImage::Format_BGR888 告诉 Qt 直接按 BGR 数据创建，无需再转 RGB
        img = QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                     QImage::Format_BGR888).copy();
        break;
    case CV_8UC1:
        img = QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                     QImage::Format_Grayscale8).copy();
        break;
    default://其他类型，如 CV_32F、CV_16U 等，QImage 不直接支持，需要先转换
    {
        // 尝试将其他类型转为 8UC3
        cv::Mat tmp;
        if (mat.channels() == 1)
            mat.convertTo(tmp, CV_8UC1, 1.0, 0);//tmp(x,y) = mat(x,y) * alpha + beta
        else
        {
            cv::cvtColor(mat, tmp, cv::COLOR_BGR2RGB);//先BGR转 RGB，再转 8UC3
            tmp.convertTo(tmp, CV_8UC3, 1.0, 0);
        }
        if (!tmp.empty())
            img = cvMatToQImage(tmp); // 递归一次，类型已确定
        return img;
    }
    }

    return img;
}

void MainWindow::showMatOnLabel(QLabel* label, const cv::Mat& mat) const
{
    if (!label || mat.empty())
        return;

    cv::Mat display = mat.clone();

    // 应用亮度/对比度调整（仅影响显示，不写回算法结果）
    if (m_brightness != 0 || m_contrast != 0)
    {
        // 先转 float 做线性变换，避免溢出
        cv::Mat tmp;
        display.convertTo(tmp, CV_32F);

        // 亮度：直接加减
        tmp += static_cast<float>(m_brightness);

        // 对比度：以 128 为中心缩放
        float contrastFactor = 1.0f + m_contrast / 100.0f;
        tmp = (tmp - 128.0f) * contrastFactor + 128.0f;

        // clamp 到 [0, 255]
        tmp = cv::max(tmp, 0.0f);
        tmp = cv::min(tmp, 255.0f);

        tmp.convertTo(display, mat.type());
    }

    QImage img = cvMatToQImage(display);
    if (img.isNull())
        return;

    label->setPixmap(QPixmap::fromImage(img));
    label->setScaledContents(true);
}

// ============================================================================
//  观察 ROI 鼠标框选
// ============================================================================

// 1. 绘制红色虚线框
void MainWindow::drawRoiRect()
{
    if (!m_isSelectingObserveRoi || !m_observeRoiEnabled)
        return;

    // QPainter 直接在 label 上绘制 overlay 矩形（不碰 pixmap，无叠加）
    QPainter painter(ui->lblViewOriginal);
    QPen pen(Qt::red, 2, Qt::DashLine);
    painter.setPen(pen);
    QRect r = QRect(m_observeStartPoint, m_observeEndPoint).normalized();
    painter.drawRect(r);
}

// 2. 事件过滤器 — 捕获鼠标框选交互（Press / Move / Release / Paint）
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != ui->lblViewOriginal || !m_observeRoiEnabled)
        return QMainWindow::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress)
    {
        // 记录框选起点
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        m_isSelectingObserveRoi = true;
        m_observeStartPoint    = me->pos();
        m_observeEndPoint      = me->pos();
        return true;
    }

    if (event->type() == QEvent::MouseMove)
    {
        if (!m_isSelectingObserveRoi)
            return true;
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        m_observeEndPoint = me->pos();
        ui->lblViewOriginal->update(); // 触发 Paint 事件重绘
        return true;
    }

    if (event->type() == QEvent::MouseButtonRelease)
    {
        if (!m_isSelectingObserveRoi)
            return true;
        m_isSelectingObserveRoi = false;

        // 归一化矩形（Qt 自带 .normalized() 处理反向拖拽）
        QRect labelRect = QRect(m_observeStartPoint, m_observeEndPoint).normalized();
        if (labelRect.width() < 5 || labelRect.height() < 5)
            return true; // 太小，忽略

        // 坐标换算：QLabel 像素 → 原图像素
        QPixmap pix = ui->lblViewOriginal->pixmap();
        if (pix.isNull() || m_lastResult.frameOriginal.empty())
            return true;
        if (labelRect.width() < 5 || labelRect.height() < 5)
            return true;

        const QSize imgSize(m_lastResult.frameOriginal.cols,
                            m_lastResult.frameOriginal.rows);
        const int lblW = ui->lblViewOriginal->width();
        const int lblH = ui->lblViewOriginal->height();
        if (lblW == 0 || lblH == 0)
            return true;

        double ratioX = (double)imgSize.width()  / lblW;
        double ratioY = (double)imgSize.height() / lblH;

        // clamp 到图像边界
        QRect roi;
        roi.setX(  qBound(0, (int)(labelRect.x()      * ratioX), imgSize.width()  - 1));
        roi.setY(  qBound(0, (int)(labelRect.y()      * ratioY), imgSize.height() - 1));
        roi.setRight( qBound(0, (int)(labelRect.right()  * ratioX), imgSize.width()  - 1));
        roi.setBottom(qBound(0, (int)(labelRect.bottom() * ratioY), imgSize.height() - 1));

        m_observeRoiRect = roi;

        // 先确保弹窗已创建，再刷新裁切图（保证首次框选即可看到图片）
        if (!m_roiDialog)
            m_roiDialog = new RoiViewerDialog(this);
        refreshObserveRoiViews();
        updateParamLabels();
        m_roiDialog->show();
        return true;
    }

    if (event->type() == QEvent::Paint)
    {
        // QPainter 直接在 label 上绘制（overlay，不污染 pixmap）
        QPainter painter(ui->lblViewOriginal);
        // ① 先画底图
        if (!ui->lblViewOriginal->pixmap().isNull())
            painter.drawPixmap(ui->lblViewOriginal->rect(), ui->lblViewOriginal->pixmap());
        // ② 再画红框 overlay
        drawRoiRect();
        return true; // 已自行完成绘制，阻止默认行为
    }

    return QMainWindow::eventFilter(watched, event);
}
