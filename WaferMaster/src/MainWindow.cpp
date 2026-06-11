#include "MainWindow.h"
#include "ui_WaferMaster.h"
#include "FrameProducer.h"
#include "WaferAlgorithm.h"

#include <QFileDialog>
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
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setupUiState();
    setupWorkers();
    setupConnections();
}

MainWindow::~MainWindow()
{
    cleanupWorkers();
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

    // 手动 connect，使用自定义槽名（非 Qt 自动连接约定）
    connect(ui->btnBrowse,      &QPushButton::clicked, this, &MainWindow::onBrowseClicked);
    connect(ui->btnStart,       &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui->btnStop,        &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(ui->btnObserveRoi,  &QPushButton::toggled, this, &MainWindow::onObserveRoiToggled);

    connect(ui->cmbSourceType, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSourceTypeChanged);

    connect(ui->sliderBright,   &QSlider::valueChanged, this, &MainWindow::onBrightnessChanged);
    connect(ui->sliderContrast, &QSlider::valueChanged, this, &MainWindow::onContrastChanged);

    // 跨线程 Worker 连接由 onStartClicked() 全量重建，此处不连接
}

void MainWindow::cleanupWorkers()
{
    // 先停采集再停算法，确保不再有新帧入队
    if (m_producer)
    {
        // 跨线程调用 stop，由 Qt 排队执行
        QMetaObject::invokeMethod(m_producer, "stop", Qt::QueuedConnection);
    }
    if (m_algorithm)
    {
        QMetaObject::invokeMethod(m_algorithm, "stop", Qt::QueuedConnection);
    }

    // 等待线程退出（quit() 会处理完已排队的 invokeMethod 调用再退出）
    // QTimer 驱动版 stop() 立即中断，wait(500) 绰绰有余
    if (m_producerThread && m_producerThread->isRunning())
    {
        m_producerThread->quit();
        m_producerThread->wait(500);
    }
    if (m_algorithmThread && m_algorithmThread->isRunning())
    {
        m_algorithmThread->quit();
        m_algorithmThread->wait(500);
    }

    // quit + wait 后线程已停止，直接 delete Worker 安全
    // （Worker 亲和性在已退出的线程上，无事件循环处理 deleteLater）
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
    const int idx = ui->cmbSourceType->currentIndex();

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
    m_producer->setSourceConfig(m_sourceConfig);
   m_currentAlgoCfg = AlgoConfig{}; // 使用默认参数（Common.h 中的阈值）
m_algorithm->setAlgoConfig(m_currentAlgoCfg);
    m_algorithm->setFrameProducer(m_producer);

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
        ui->btnObserveRoi->blockSignals(true);
        ui->btnObserveRoi->setChecked(m_observeRoiEnabled);
        ui->btnObserveRoi->blockSignals(false);
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
        // 关闭观察 ROI：还原光标、清空 ROI 显示区域、清除原图上红框
        ui->lblViewOriginal->setCursor(Qt::ArrowCursor);
        ui->lblRoiOriginal->clear();
        ui->lblRoiResult->clear();
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
}

void MainWindow::onWorkerError(const QString& message)
{
    QMessageBox::critical(this, QStringLiteral("错误"), message);
}

void MainWindow::onProducerFinished()
{
    // 采集线程结束，不做额外操作（由 onStopClicked 统一处理状态切换）
}

void MainWindow::onAlgorithmFinished()
{
    // 算法线程结束，不做额外操作
}

// ============================================================================
// 配置与状态
// ============================================================================

SourceConfig MainWindow::buildSourceConfig() const
{
    SourceConfig cfg;

    const int idx = ui->cmbSourceType->currentIndex();
    cfg.sourceType = (idx == 0) ? InputSourceType::ImageSequence
                                : InputSourceType::AviVideo;
    cfg.sourcePath = ui->editSourcePath->text().trimmed();

    // 帧间隔暂用默认值 33ms，不从 UI 读取
    // cfg.frameIntervalMs 保持 Common.h 中定义的 33

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
            ui->lblRoiOriginal->clear();
            ui->lblRoiResult->clear();
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

    updateStatusBarText();
}

void MainWindow::updateStatusBarText()
{
    if (!ui->statusBar)
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
        return;

    // 从 m_lastResult 中裁切
    const QRect& r = m_observeRoiRect;

    // 原图 ROI
    if (!m_lastResult.frameOriginal.empty())
    {
        cv::Rect roi(r.x(), r.y(), r.width(), r.height());
        // 边界保护
        roi &= cv::Rect(0, 0, m_lastResult.frameOriginal.cols, m_lastResult.frameOriginal.rows);
        if (roi.width > 0 && roi.height > 0)
        {
            cv::Mat cropped = m_lastResult.frameOriginal(roi).clone();
            showMatOnLabel(ui->lblRoiOriginal, cropped);
        }
    }

    // 平坦图 ROI（结果图）
    if (!m_lastResult.frameFlatness.empty())
    {
        cv::Rect roi(r.x(), r.y(), r.width(), r.height());
        roi &= cv::Rect(0, 0, m_lastResult.frameFlatness.cols, m_lastResult.frameFlatness.rows);
        if (roi.width > 0 && roi.height > 0)
        {
            cv::Mat cropped = m_lastResult.frameFlatness(roi).clone();
            showMatOnLabel(ui->lblRoiResult, cropped);
        }
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
        // BGR → RGB
        img = QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                     QImage::Format_BGR888).copy();
        break;
    case CV_8UC1:
        img = QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                     QImage::Format_Grayscale8).copy();
        break;
    default:
    {
        // 尝试将其他类型转为 8UC3
        cv::Mat tmp;
        if (mat.channels() == 1)
            mat.convertTo(tmp, CV_8UC1, 1.0, 0);
        else
        {
            cv::cvtColor(mat, tmp, cv::COLOR_BGR2RGB);
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
// Phase 2 — 观察 ROI 鼠标框选（原项目 QtWidgetsApplication1 简化迁移）
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

        // 刷新裁切小图 + 参数标签
        refreshObserveRoiViews();
        updateParamLabels();
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
