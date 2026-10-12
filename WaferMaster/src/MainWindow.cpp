#include "MainWindow.h"
#include "ui_WaferMaster.h"
#include "FrameProducer.h"
#include "WaferAlgorithm.h"
#include "RoiViewerDialog.h"
#include "CommunicationManager.h"
#include "Logger.h"
#include "ResultLogger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QHostAddress>
#include <QMessageBox>
#include <QPixmap>
#include <QLabel>
#include <QStatusBar>
#include <QPainter>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QInputDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QDateTime>

// ============================================================================
// 构造 & 析构
// ============================================================================

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)//创建一个 Ui::MainWindow 对象，并将其地址赋值给成员指针 ui。
{
    ui->setupUi(this);//调用 ui 对象的 setupUi() 方法，传入 this 指针（MainWindow 实例），完成 UI 初始化。
    this->setWindowIcon(QIcon(":/logo.png"));
    setupUiState();//设置控件初始值
    setupWorkers();//创建线程对象
    setupConnections();//建立信号槽连接
    setupCommunication();//创建并启动 TCP 通信服务器
    initResultLogger(StorageMode::Both); // 硬编码默认 CSV + SQLite 双写
}

MainWindow::~MainWindow()
{
    cleanupWorkers();
    if (m_comm) {
        m_comm->stopServer();
    }
    delete m_roiDialog;//防御性删除，确保即使未创建也安全
    cleanupResultLogger(); // 等待已经接受的记录保存完成，不直接删除跨线程 Worker
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

    connect(ui->cmbDetectionMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDetectionModeChanged);

    connect(ui->sliderBright,   &QSlider::valueChanged, this, &MainWindow::onBrightnessChanged);
    connect(ui->sliderContrast, &QSlider::valueChanged, this, &MainWindow::onContrastChanged);

    connect(ui->btnQueryHistory, &QPushButton::clicked,this, &MainWindow::onQueryHistoryClicked);      

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
    // 信号①：远程 START → 模拟本地点击"开始检测"
    connect(m_comm, &CommunicationManager::startRequested,
            this, &MainWindow::onCommStartRequested);
    // 信号②：远程 STOP → 模拟本地点击"停止检测"
    connect(m_comm, &CommunicationManager::stopRequested,
            this, &MainWindow::onCommStopRequested);
    // 信号③：监听状态变化 → 刷新 UI 通信面板
    connect(m_comm, &CommunicationManager::listeningStateChanged,
            this, [this](bool listening, const QString& msg) {
        ui->lblCommListenState->setText(
            QStringLiteral("状态: %1").arg(listening
                ? QStringLiteral("监听中") : msg));
        ui->lblCommMode->setText(QStringLiteral("模式: TCP Server"));
        ui->lblCommAddress->setText(QStringLiteral("地址: 127.0.0.1"));
        ui->lblCommPort->setText(QStringLiteral("端口: 9000"));
    });
    // 信号④：客户端连接/断开 → 刷新客户端数量
    connect(m_comm, &CommunicationManager::clientStateChanged,
            this, [this](bool connected, int count) {
        ui->lblCommClientState->setText(
            QStringLiteral("客户端: %1").arg(connected
                ? QStringLiteral("%1个已连接").arg(count)
                : QStringLiteral("未连接")));
    });
    // 信号⑤：通信日志 → 追加到日志面板
    connect(m_comm, &CommunicationManager::logMessage,
            this, &MainWindow::onLogMessage);
    m_comm->startServer(QHostAddress::LocalHost, 9000);//启动 TCP 服务器，监听本地回环地址
}

void MainWindow::buildDeepLearning()
{
    // 模型路径：exe 同目录的 models/ 下，由 CMake POST_BUILD 自动复制
    m_dlconfig.modelPath = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/models/model_curved_fringe.onnx");

    // 主线程只传配置，实际加载由算法线程的 start() 完成。
    m_algorithm->setDLConfig(m_dlconfig);
    m_algorithm->setDetectionMode(m_detectionMode);
}

// ═══════════════════════════════════════════════════════════════════
// 结果记录初始化：CSV + SQLite 双写
// ═══════════════════════════════════════════════════════════════════
void MainWindow::initResultLogger(StorageMode mode,
                                  const QString& csvPath,
                                  const QString& dbPath)
{
    cleanupResultLogger();
    m_storageMode = mode;
    m_storageReady = false;
    m_storageFailed = false;
    m_storageThread = new QThread(this);
    m_storageThread->setObjectName(QStringLiteral("ResultStorage"));
    m_resultLogger = new ResultLogger(); // 无父对象，才能 moveToThread
    m_resultLogger->moveToThread(m_storageThread);

    // 若未指定路径，默认写入 exe 所在目录的 logs/ 子目录
    const QString logDir = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
    // 创建目录和打开文件也交给存储线程，不在主线程执行磁盘操作。
    //  三目运算：如果调用者没传路径（空字符串），就用默认路径
    const QString finalCsv = csvPath.isEmpty()
        ? logDir + QStringLiteral("/records_v2.csv") : csvPath; // 新格式另存，旧 records.csv 不动
    const QString finalDb = dbPath.isEmpty()
        ? logDir + QStringLiteral("/records.db")  : dbPath;

    // 先连接通知，再启动线程；数据库在存储线程中创建、使用和移除。
    connect(m_resultLogger, &ResultLogger::logMessage,
            this, &MainWindow::onLogMessage);
    connect(m_resultLogger, &ResultLogger::errorOccurred,
            this, &MainWindow::onStorageError, Qt::QueuedConnection);
    connect(m_storageThread, &QThread::finished, m_resultLogger, &QObject::deleteLater);
    m_storageThread->start();
    // 只在初始化时等待；文件和数据库仍在存储线程打开，之后每帧保存不等待。
    QMetaObject::invokeMethod(m_resultLogger, [this, mode, finalCsv, finalDb] {
        m_storageReady = m_resultLogger->setMode(mode, finalCsv, finalDb);
    }, Qt::BlockingQueuedConnection);
    ui->btnStart->setEnabled(m_storageReady);
}

void MainWindow::cleanupResultLogger()
{
    if (m_resultLogger && m_storageThread && m_storageThread->isRunning())
    {
        // 主线程已不再提交记录；本次调用排在保存/查询任务之后，避免 quit 丢掉积压任务。
        // Worker 不反向阻塞调用主线程，因此这里不会互相等待。
        QMetaObject::invokeMethod(m_resultLogger, [logger = m_resultLogger] {
            logger->close();
        }, Qt::BlockingQueuedConnection);
        m_storageThread->quit();
        m_storageThread->wait();
    }
    m_resultLogger = nullptr; // 线程 finished → deleteLater 已回收 Worker
    delete m_storageThread;
    m_storageThread = nullptr;
    m_storageReady = false;
}

void MainWindow::cleanupWorkers()
{
    // 本函数只在主线程调用。清理期间不再接收 Worker 的业务通知。
    // 线程 finished → Worker deleteLater 的连接仍然保留。
    if (m_producer)
        disconnect(m_producer, nullptr, this, nullptr);

    if (m_algorithm)
        disconnect(m_algorithm, nullptr, this, nullptr);

    // 先请求算法停止取帧，不用等待算法线程的事件队列。
    if (m_algorithm)
        m_algorithm->requestStop();

    if (m_producer &&
        m_producerThread &&
        m_producerThread->isRunning())
    {
        // 在采集线程中停止定时器并清空队列，暂时保留生产者对象。
        QMetaObject::invokeMethod(m_producer,
                                  &FrameProducer::stop,
                                  Qt::BlockingQueuedConnection);
    }

    // 算法还持有生产者指针，必须先结束算法线程，再销毁生产者。
    // wait() 会阻塞主线程，需等待当前模型加载、读取或推理返回。
    if (m_algorithmThread)
    {
        m_algorithmThread->quit();
        m_algorithmThread->wait();
    }
    m_algorithm = nullptr; // Worker 已通过线程 finished → deleteLater 回收

    if (m_producerThread)
    {
        m_producerThread->quit();
        m_producerThread->wait();
    }
    m_producer = nullptr;

    // QThread 对象属于主线程，对应线程已经退出，可以直接删除。
    delete m_algorithmThread;
    m_algorithmThread = nullptr;
    delete m_producerThread;
    m_producerThread = nullptr;
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
  ├─ connect                    建跨线程信号链和 Worker 自动回收连接
  ├─ setSourceConfig()          注入配置
  ├─ setFrameProducer()         算法绑定生产者
  ├─ updateRunState(Running)    状态切为运行中
  └─ thread->start() × 2        启动线程，算法线程内加载模型
*/
void MainWindow::onStartClicked()
{
    if (!m_storageReady || m_storageFailed)
    {
        QMessageBox::warning(this, QStringLiteral("结果存储"),
            m_storageFailed
                ? QStringLiteral("结果存储发生过错误。请修复磁盘空间、路径或权限后重新启动程序。")
                : QStringLiteral("结果存储未就绪。请检查路径和权限后重新启动程序。"));
        return;
    }
    if (m_runState == RunState::Running)
        return;

    // 从 UI 收参
    m_sourceConfig = buildSourceConfig();

    // 路径非空校验
    if (m_sourceConfig.sourcePath.isEmpty()||
    m_sourceConfig.sourcePath == ui->editSourcePath->placeholderText())//防止路径被错误地设置成了占位文本
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
 
    buildDeepLearning(); // 保存模型配置和检测模式，不在主线程加载模型

    // 迁移到各自线程
    m_producer->moveToThread(m_producerThread);
    m_algorithm->moveToThread(m_algorithmThread);

    // 线程真正结束后，在各自线程中销毁 Worker。
    // Worker 的 finished 只通知业务结束，quit() 统一交给 cleanupWorkers()。
    connect(m_producerThread, &QThread::finished,
            m_producer, &QObject::deleteLater);
    connect(m_algorithmThread, &QThread::finished,
            m_algorithm, &QObject::deleteLater);

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

    updateRunState(RunState::Running);

    // 先启动算法线程；加载模型期间，采集帧通过原有队列等待处理。
    m_algorithmThread->start();
    m_producerThread->start();
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
    // 忽略停止后或上一轮留下的源信息。
    if (!m_producer || sender() != m_producer || m_runState != RunState::Running)
        return;

    m_currentPath       = path;
    m_currentResolution = resolution;
    updateStatusBarText();
}

void MainWindow::onAlgorithmResultReady(const AlgoResult& result)
{
    // 停止后或上一轮遗留的结果，不再刷新界面和写入记录。
    if (!m_algorithm || sender() != m_algorithm || m_runState != RunState::Running)
        return;

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

    // 只提交保存任务；CSV、SQLite 和 PNG 写入由存储线程执行。
    if (m_resultLogger)
        m_resultLogger->enqueue(result);
}//Algorithm每处理完一帧后，都会调用这个槽函数来更新界面显示和状态栏信息。

void MainWindow::onWorkerError(const QString& message)
{
    QObject* source = sender();
    // 只处理本轮 Worker 的首个错误，避免重复弹窗和重复清理。
    if (!source ||
        (source != m_producer && source != m_algorithm) ||
        m_runState != RunState::Running)
    {
        return;
    }

    updateRunState(RunState::Error);
    cleanupWorkers();

    // 先清理再弹窗，避免模态对话框显示期间后台仍在运行。
    QMessageBox::critical(this, QStringLiteral("错误"), message);
}

void MainWindow::onProducerFinished()
{
    if (!m_producer || sender() != m_producer || m_runState != RunState::Running)
        return;

    // 采集读完不立即清空队列，让算法处理剩余帧后再通知结束。
    if (m_algorithm)
    {
        QMetaObject::invokeMethod(m_algorithm,
                                  &WaferAlgorithm::finishInput,
                                  Qt::QueuedConnection);
    }
}

void MainWindow::onAlgorithmFinished()
{
    if (!m_algorithm || sender() != m_algorithm || m_runState != RunState::Running)
        return;

    cleanupWorkers();
    updateRunState(RunState::Stopped);
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
    // Error 状态也已完成清理，修正输入后允许重新启动。
    if (m_runState == RunState::Idle ||
        m_runState == RunState::Stopped ||
        m_runState == RunState::Error)
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

// ═══════════════════════════════════════════════════════════════════
// 历史查询
// ═══════════════════════════════════════════════════════════════════

void MainWindow::onQueryHistoryClicked()
{
    if (!m_resultLogger || !m_storageReady ||
        (m_storageMode != StorageMode::Sqlite && m_storageMode != StorageMode::Both))
    {
        QMessageBox::warning(this,
            QStringLiteral("提示"),
            QStringLiteral("存储尚未就绪，或当前模式不支持历史查询。"));
        return;
    }

    QStringList levels{QStringLiteral("Good"),
                       QStringLiteral("Warning"),
                       QStringLiteral("NG")};
    bool ok = false;
    QString chosen = QInputDialog::getItem(this,
        QStringLiteral("查询历史记录"),
        QStringLiteral("选择检测等级："), levels, 0, false, &ok);
    if (!ok || chosen.isEmpty()) return;

    //选择的字符串转换为 DetectionLevel 枚举值
    DetectionLevel level = DetectionLevel::Good;
    if      (chosen == QStringLiteral("Warning")) level = DetectionLevel::Warning;
    else if (chosen == QStringLiteral("NG"))      level = DetectionLevel::Ng;

    QList<AlgoResult> results;
    QString error;
    // 手动查询才等待：SQL 仍在存储线程执行，不能从主线程直接使用它的数据库。
    // 查询会排在已提交的保存任务之后；数据较多或磁盘较慢时，界面会短暂等待。
    QMetaObject::invokeMethod(m_resultLogger, [this, level, &results, &error] {
        results = m_resultLogger->queryByLevel(level, error);
    }, Qt::BlockingQueuedConnection);
    if (!error.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("历史查询失败"), error);
        return;
    }
    if (results.isEmpty())
    {
        QMessageBox::information(this,
            QStringLiteral("查询结果"),
            QStringLiteral("未找到等级为 %1 的历史记录").arg(chosen));
        return;
    }
    //创建表格对话框
    QDialog* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);//用户关闭时自动删除，避免内存泄漏
    dlg->setWindowTitle(QStringLiteral("历史查询 — %1（最近%2条，最多1000条）")
                        .arg(chosen).arg(results.size()));
    dlg->resize(1100, 450);
    // 新旧记录一起显示；旧记录无法恢复的模式/分数显示为未知或 --。
    QTableWidget* table = new QTableWidget(results.size(), 10, dlg);
    table->setHorizontalHeaderLabels({
        QStringLiteral("保存时间"), QStringLiteral("帧号"), QStringLiteral("模式"),
        QStringLiteral("FI"), QStringLiteral("P95"), QStringLiteral("HotRatio(%)"),
        QStringLiteral("Score"), QStringLiteral("耗时(ms)"), QStringLiteral("Level"),
        QStringLiteral("热力图路径")});//设置表头标签
    table->horizontalHeader()->setStretchLastSection(true);//最后一列自动拉伸填满剩余宽度
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);//禁止编辑
    table->setSelectionBehavior(QAbstractItemView::SelectRows);//整行选中

    for (int i = 0; i < results.size(); ++i)//遍历查询结果，填充表格
    {
        const AlgoResult& r = results[i];
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(r.recordedAtMs);
        const bool dl = r.hasModeInfo && r.mode == DetectionMode::DeepLearning;
        table->setItem(i, 0,
            new QTableWidgetItem(dt.toString(QStringLiteral("MM-dd hh:mm:ss"))));
        table->setItem(i, 1,
            new QTableWidgetItem(QString::number(r.frameIdx)));
        table->setItem(i, 2,
            new QTableWidgetItem(r.hasModeInfo ? detectionModeToString(r.mode) : QStringLiteral("未知（旧记录）")));
        table->setItem(i, 3,
            new QTableWidgetItem(dl ? QStringLiteral("--") : QString::number(r.fi, 'f', 2)));
        table->setItem(i, 4,
            new QTableWidgetItem(dl ? QStringLiteral("--") : QString::number(r.p95, 'f', 1)));
        table->setItem(i, 5,
            new QTableWidgetItem(dl ? QStringLiteral("--") : QString::number(r.hotRatio, 'f', 2)));
        table->setItem(i, 6,
            new QTableWidgetItem(dl ? QString::number(r.anomalyScore, 'f', 3) : QStringLiteral("--")));
        table->setItem(i, 7,
            new QTableWidgetItem(dl ? QString::number(r.inferenceTimeMs) : QStringLiteral("--")));
        table->setItem(i, 8,
            new QTableWidgetItem(detectionLevelToString(r.level)));
        table->setItem(i, 9, new QTableWidgetItem(r.heatmapPath));
    }
    //创建布局管理器对象
    QVBoxLayout* lay = new QVBoxLayout(dlg);
    lay->addWidget(table);//把表格控件添加到布局中
    dlg->exec();
}/*这些对象的生命周期从函数调用到dlg关闭，不存在"声明后、赋值前被使用"的窗口期，不需初始化*/

void MainWindow::onStorageError(const QString& message)
{
    if (sender() != m_resultLogger || m_storageFailed) return;
    m_storageFailed = true;
    updateRunState(RunState::Error);
    cleanupWorkers(); // 只停止检测，不清掉已经接受的存储任务
    ui->btnStart->setEnabled(false);
    QMessageBox::critical(this, QStringLiteral("结果存储错误"), message);
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
        ui->cmbDetectionMode->setEnabled(true);
        break;
    case RunState::Running:
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(true);
        ui->btnBrowse->setEnabled(false);
        ui->cmbDetectionMode->setEnabled(false);
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
        ui->cmbDetectionMode->setEnabled(true);
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

    // 按模式显示指标
    if (m_lastResult.mode == DetectionMode::DeepLearning)
    {
        if (!text.isEmpty()) text += QStringLiteral(" | ");
        text += QStringLiteral("Score: %1").arg(m_lastResult.anomalyScore, 0, 'f', 3);
        if (!text.isEmpty()) text += QStringLiteral(" | ");
        text += QStringLiteral("%1ms").arg(m_lastResult.inferenceTimeMs);
    }
    else
    {
        // FI
        if (!text.isEmpty()) text += QStringLiteral(" | ");
        text += QStringLiteral("FI: %1").arg(m_lastResult.fi, 0, 'f', 2);

        // P95
        if (!text.isEmpty()) text += QStringLiteral(" | ");
        text += QStringLiteral("P95: %1").arg(m_lastResult.p95, 0, 'f', 1);

        // HotRatio
        if (!text.isEmpty()) text += QStringLiteral(" | ");
        text += QStringLiteral("Hot: %1%").arg(m_lastResult.hotRatio, 0, 'f', 1);
    }

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
    // —— DL 模式：显示 DL 参数 ——
    if (m_detectionMode == DetectionMode::DeepLearning)
    {
        // 尚未收到 DL 结果时显示占位符，避免把默认值误认为推理结果。
        const bool hasDlResult =
            m_lastResult.mode == DetectionMode::DeepLearning &&
            !m_lastResult.frameOriginal.empty();
        ui->lblDLScore->setText(
            hasDlResult
                ? QStringLiteral("AnomalyScore: %1")
                      .arg(m_lastResult.anomalyScore, 0, 'f', 3)
                : QStringLiteral("AnomalyScore: -"));
        ui->lblDLGoodTh->setText(
            QStringLiteral("Good阈值: %1")
                .arg(m_dlconfig.goodThreshold, 0, 'f', 3));
        ui->lblDLNgTh->setText(
            QStringLiteral("NG阈值: %1")
                .arg(m_dlconfig.ngThreshold, 0, 'f', 3));
        ui->lblDLInferMs->setText(
            hasDlResult
                ? QStringLiteral("推理耗时: %1 ms")
                      .arg(m_lastResult.inferenceTimeMs)
                : QStringLiteral("推理耗时: - ms"));
        if (m_observeRoiEnabled && !m_observeRoiRect.isNull())
        {
            ui->lblDLObserveRoi->setText(
                QStringLiteral("观察ROI: (%1,%2) %3x%4")
                    .arg(m_observeRoiRect.x())
                    .arg(m_observeRoiRect.y())
                    .arg(m_observeRoiRect.width())
                    .arg(m_observeRoiRect.height()));
        }
        else
        {
            ui->lblDLObserveRoi->setText(QStringLiteral("观察ROI: -"));
        }
        return;
    }

    // —— 传统模式：显示 OpenCV 参数 ——
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
// 检测模式切换（仅 UI 层：三视图标题 + 参数面板切换）
// ============================================================================

void MainWindow::onDetectionModeChanged(int index)
{
    if (m_runState == RunState::Running)
        return;

    m_detectionMode = (index == 0) ? DetectionMode::Traditional
                                   : DetectionMode::DeepLearning;

    updateViewLabelsForMode(m_detectionMode);
    updateParamLabels();
}

void MainWindow::updateViewLabelsForMode(DetectionMode mode)
{
    bool isDL = (mode == DetectionMode::DeepLearning);

    ui->groupSpectrum->setTitle(isDL ? "异常热力图" : "频谱图");
    ui->groupFlatness->setTitle(isDL ? "模型检测结果" : "平坦图");

    ui->groupParams->setVisible(!isDL);
    ui->groupDLParams->setVisible(isDL);
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

    QPixmap pixmap = QPixmap::fromImage(img);
    label->setPixmap(pixmap.scaled(
        label->size(),
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation));//把图片缩放到label当前大小范围内，保持宽高比，使用平滑算法
    label->setAlignment(Qt::AlignCenter);//居中显示
    label->setScaledContents(false);//禁止 QLabel 自动把 pixmap 强行拉满整个控件
}

// ============================================================================
// 窗口尺寸变化：全屏 / 恢复时重新缩放所有图像
// ============================================================================

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);

    if (!m_lastResult.frameOriginal.empty())
    {
        refreshMainViews(m_lastResult);

        if (m_observeRoiEnabled && !m_observeRoiRect.isNull())
            refreshObserveRoiViews();

        ui->lblViewOriginal->update();
    }
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

// 2. 计算 QLabel 内 pixmap 的实际显示区域（KeepAspectRatio 居中后的矩形）
QRect MainWindow::imageDisplayRect(QLabel* label) const
{
    if (!label)
        return QRect();

    const QPixmap pix = label->pixmap();
    if (pix.isNull())
        return QRect();

    const QSize szPix = pix.size();
    const QSize szLbl = label->size();

    const int x = (szLbl.width()  - szPix.width())  / 2;
    const int y = (szLbl.height() - szPix.height()) / 2;

    return QRect(x, y, szPix.width(), szPix.height());
}

// 3. 事件过滤器 — 捕获鼠标框选交互（Press / Move / Release / Paint）
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

        // 坐标换算：QLabel 像素 → 原图像素（通过 imageDisplayRect 扣除留白 offset）
        QPixmap pix = ui->lblViewOriginal->pixmap();
        if (pix.isNull() || m_lastResult.frameOriginal.empty())
            return true;

        QRect displayRect = imageDisplayRect(ui->lblViewOriginal);
        if (displayRect.width() < 5 || displayRect.height() < 5)
            return true;

        // 将 label 坐标系下的框选矩形与 pixmap 实际显示区域求交集
        // 避免在黑色留白区拖框产生错误的 ROI
        QRect clippedRect = labelRect.intersected(displayRect);
        if (clippedRect.width() < 5 || clippedRect.height() < 5)
            return true;

        const QSize imgSize(m_lastResult.frameOriginal.cols,
                            m_lastResult.frameOriginal.rows);

        // 比例因子：原图尺寸 / pixmap 显示尺寸
        double ratioX = (double)imgSize.width()  / displayRect.width();
        double ratioY = (double)imgSize.height() / displayRect.height();

        // 扣除 displayRect 的左上角 offset 后按比例换算到图像坐标
        QRect roi;
        roi.setX(  qBound(0, (int)((clippedRect.x()      - displayRect.x()) * ratioX), imgSize.width()  - 1));
        roi.setY(  qBound(0, (int)((clippedRect.y()      - displayRect.y()) * ratioY), imgSize.height() - 1));
        roi.setRight( qBound(0, (int)((clippedRect.right()  - displayRect.x()) * ratioX), imgSize.width()  - 1));
        roi.setBottom(qBound(0, (int)((clippedRect.bottom() - displayRect.y()) * ratioY), imgSize.height() - 1));

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
        // ① 先画底图（居中绘制到 pixmap 的实际显示区域，保持宽高比不变形）
        const QPixmap pix = ui->lblViewOriginal->pixmap();
        if (!pix.isNull())
            painter.drawPixmap(imageDisplayRect(ui->lblViewOriginal), pix);
        // ② 再画红框 overlay
        drawRoiRect();
        return true; // 已自行完成绘制，阻止默认行为
    }

    return QMainWindow::eventFilter(watched, event);
}
