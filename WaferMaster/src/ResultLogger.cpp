#include "ResultLogger.h"
#include "Logger.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QUuid>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>

namespace {
const QByteArray csvHeader = QByteArray("ts,frame,fi,p95,hotRatio,level,roi_x,roi_y,roi_w,roi_h") +
    ",mode,score,inference_ms,frame_ts,heatmap_path,heatmap_w,heatmap_h";

QString savedTime(const AlgoResult& r)
{
    return QDateTime::fromMSecsSinceEpoch(r.recordedAtMs).toString(Qt::ISODateWithMs);
}

// 路径可能含逗号或双引号，CSV 必须加引号并把内部双引号写成两个。
QString csvQuoted(QString value)
{
    value.replace('"', QStringLiteral("\"\""));
    return '"' + value + '"';
}
}

ResultLogger::ResultLogger(QObject* parent)
    : QObject(parent), m_csvFile(this)
{
    // QFile 也是 QObject：设为子对象，moveToThread 时才能一起迁移。
}

ResultLogger::~ResultLogger()
{
    close(); // Worker 在线程内销毁，文件和数据库也在线程内收尾
}

void ResultLogger::reportError(const QString& message, const QString& detail)
{
    Logger::get()->error("Result storage failed: {}", detail.toUtf8().toStdString());
    // 后续待写任务不再生成半成品；同一轮保存故障只弹一次错误。
    if (!m_failed.exchange(true))
        emit errorOccurred(message + QStringLiteral("\n技术详情：") + detail);
}

bool ResultLogger::setMode(StorageMode mode, const QString& csvPath, const QString& dbPath)
{
    close();
    m_failed.store(false);
    m_mode = mode;
    const QString basePath = mode == StorageMode::Sqlite ? dbPath : csvPath;
    m_heatmapDir = QFileInfo(basePath).absolutePath() + QStringLiteral("/heatmaps");

    if ((mode == StorageMode::Csv || mode == StorageMode::Both) && !initCsv(csvPath))
        return false;
    if ((mode == StorageMode::Sqlite || mode == StorageMode::Both) && !initSqlite(dbPath))
        return false;
    emit logMessage(QStringLiteral("结果存储已就绪。"));
    return true;
}

bool ResultLogger::initCsv(const QString& path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        reportError(QStringLiteral("无法创建 CSV 目录，请检查路径和写入权限。"), path);
        return false;
    }

    m_csvFile.setFileName(path);
    if (!m_csvFile.open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Text))
    {
        reportError(QStringLiteral("无法打开 CSV，请检查文件权限或是否被占用。"), m_csvFile.errorString());
        return false;
    }
    // 默认写 records_v2.csv。若自定义路径指向旧格式，也不能混写两种字段。
    if (m_csvFile.size() > 0)
    {
        m_csvFile.seek(0); // Append 打开后位置在末尾，先回到开头检查表头
        QByteArray header = m_csvFile.readLine().trimmed();
        if (header.startsWith("\xEF\xBB\xBF")) header.remove(0, 3);
        if (header != csvHeader)
        {
            reportError(QStringLiteral("CSV 表头不兼容，请改用新的文件路径。原文件未改动。"), path);
            m_csvFile.close();
            return false;
        }
        m_csvFile.seek(m_csvFile.size()); // 检查完成，回到末尾继续追加
    }
    m_csvStream.setDevice(&m_csvFile);
    m_csvStream.resetStatus();
    if (m_csvFile.size() == 0)
    {
        m_csvStream << QString::fromUtf8(csvHeader) << '\n';
        if (!flushCsv()) return false;
    }
    Logger::get()->info("ResultLogger: CSV ready: {}", path.toUtf8().toStdString());
    return true;
}

bool ResultLogger::initSqlite(const QString& path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        reportError(QStringLiteral("无法创建数据库目录，请检查路径和写入权限。"), path);
        return false;
    }
    m_connectionName = QStringLiteral("ResultLogger_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open())
    {
        reportError(QStringLiteral("无法打开 SQLite 数据库，请检查驱动、路径和权限。"), m_db.lastError().text());
        return false;
    }

    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS records ("
        "ts TEXT, frame INTEGER, fi REAL, p95 REAL, hotRatio REAL, level TEXT, "
        "roi_x INTEGER, roi_y INTEGER, roi_w INTEGER, roi_h INTEGER)")) ||
        !q.exec(QStringLiteral("SELECT ts,frame,fi,p95,hotRatio,level,roi_x,roi_y,roi_w,roi_h FROM records LIMIT 0")))
    {
        reportError(QStringLiteral("初始化数据库表失败，旧记录不会被删除。"), q.lastError().text());
        return false;
    }

    // CREATE TABLE 不会升级旧表；只补缺少的列，失败后重启也可以接着补。
    const QSqlRecord columns = m_db.record(QStringLiteral("records"));
    const QStringList additions{
        QStringLiteral("mode TEXT"), QStringLiteral("score REAL"),
        QStringLiteral("inference_ms INTEGER"), QStringLiteral("frame_ts INTEGER"),
        QStringLiteral("heatmap_path TEXT"), QStringLiteral("heatmap_w INTEGER"),
        QStringLiteral("heatmap_h INTEGER")};
    for (const QString& definition : additions)
    {
        if (columns.indexOf(definition.section(' ', 0, 0)) < 0 &&
            !q.exec(QStringLiteral("ALTER TABLE records ADD COLUMN ") + definition))
        {
            reportError(QStringLiteral("补充数据库字段失败，旧记录不会被删除。"), q.lastError().text());
            return false;
        }
    }
    // 旧记录新增字段保留 NULL；不能把以前的 DL 记录猜成传统模式。
    Logger::get()->info("ResultLogger: SQLite ready: {}", path.toUtf8().toStdString());
    return true;
}

bool ResultLogger::enqueue(const AlgoResult& result)
{
    if (m_failed.load()) return false;
    if (m_pending.fetch_add(1) >= MaxPendingRecords)
    {
        m_pending.fetch_sub(1);
        Logger::get()->error("Result storage queue is full; new record rejected");
        // 只拒绝本条。已接受的任务仍会保存，由主窗口停止检测防止继续积压。
        emit errorOccurred(QStringLiteral("保存队列已满，本帧未接受保存。已请求停止检测；请检查磁盘速度或可用空间。"));
        return false;
    }
    try
    {
        AlgoResult record = result;
        record.recordedAtMs = QDateTime::currentMSecsSinceEpoch();
        // 只保留数字和一张热力图，不让队列长期占用三张显示图的内存。
        record.frameOriginal.release();
        record.frameSpectrum.release();
        record.frameFlatness.release();
        record.frameHeatmap = result.frameHeatmap.clone();
        if (QMetaObject::invokeMethod(this, [this, record] {
            append(record);
            m_pending.fetch_sub(1);
        }, Qt::QueuedConnection)) return true;
    }
    catch (const std::exception& e)
    {
        Logger::get()->error("Cannot queue result: {}", e.what());
    }
    m_pending.fetch_sub(1);
    emit errorOccurred(QStringLiteral("无法提交保存任务，本帧未保存。请检查可用内存并重新启动程序。"));
    return false;
}

bool ResultLogger::saveHeatmap(AlgoResult& result)
{
    result.heatmapPath.clear();
    result.heatmapSize = QSize();
    if (result.mode != DetectionMode::DeepLearning) return true;
    const cv::Mat& mat = result.frameHeatmap;
    if (mat.empty() || mat.type() != CV_8UC3)
    {
        reportError(QStringLiteral("深度学习结果缺少有效的彩色热力图，本帧未保存。"), QStringLiteral("Expected non-empty CV_8UC3 heatmap"));
        return false;
    }
    if (!QDir().mkpath(m_heatmapDir))
    {
        reportError(QStringLiteral("无法创建热力图目录，请检查写入权限。"), m_heatmapDir);
        return false;
    }
    const QString path = m_heatmapDir + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".png");
    QSaveFile file(path);
    const QImage image(mat.data, mat.cols, mat.rows, static_cast<qsizetype>(mat.step), QImage::Format_BGR888);
    // Qt 支持中文路径；成功提交后才留下完整 PNG，不生成半张图片。
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
    {
        reportError(QStringLiteral("保存热力图失败，请检查磁盘空间和权限。"), path + ": " + file.errorString());
        return false;
    }
    result.heatmapPath = path;
    result.heatmapSize = QSize(mat.cols, mat.rows);
    return true;
}

void ResultLogger::append(AlgoResult result)
{
    if (m_failed.load() || m_mode == StorageMode::None) return;
    if (!saveHeatmap(result)) return;
    if (m_mode == StorageMode::Csv || m_mode == StorageMode::Both)
        appendCsv(result);
    if (!m_failed.load() && (m_mode == StorageMode::Sqlite || m_mode == StorageMode::Both))
        appendSqlite(result);
}

bool ResultLogger::flushCsv()
{
    m_csvStream.flush();
    if (m_csvStream.status() == QTextStream::Ok && m_csvFile.flush()) return true;
    reportError(QStringLiteral("CSV 写入失败，请检查磁盘空间和写入权限。"), m_csvFile.errorString());
    return false;
}

void ResultLogger::appendCsv(const AlgoResult& r)
{
    const bool dl = r.mode == DetectionMode::DeepLearning;
    m_csvStream << savedTime(r) << ',' << r.frameIdx << ',' << r.fi << ',' << r.p95 << ','
        << r.hotRatio << ',' << detectionLevelToString(r.level) << ','
        << r.algoRoiRect.x() << ',' << r.algoRoiRect.y() << ','
        << r.algoRoiRect.width() << ',' << r.algoRoiRect.height() << ','
        << detectionModeToString(r.mode) << ',';
    if (dl) m_csvStream << QString::number(r.anomalyScore, 'g', 17);
    m_csvStream << ',';
    if (dl) m_csvStream << r.inferenceTimeMs;
    m_csvStream << ',' << r.timestampMs << ',' << csvQuoted(r.heatmapPath) << ','
        << qMax(0, r.heatmapSize.width()) << ',' << qMax(0, r.heatmapSize.height()) << '\n';
    flushCsv(); // 保留逐条刷新，但磁盘操作现在不占用主线程
}

void ResultLogger::appendSqlite(const AlgoResult& r)
{
    const bool dl = r.mode == DetectionMode::DeepLearning;
    QSqlQuery q(m_db);
    // 明确列名，避免扩展旧表后把数据写错位置。
    const bool prepared = q.prepare(
        "INSERT INTO records (ts,frame,fi,p95,hotRatio,level,roi_x,roi_y,roi_w,roi_h,"
        "mode,score,inference_ms,frame_ts,heatmap_path,heatmap_w,heatmap_h) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    q.bindValue(0, savedTime(r));
    q.bindValue(1, r.frameIdx);
    q.bindValue(2, r.fi);
    q.bindValue(3, r.p95);
    q.bindValue(4, r.hotRatio);
    q.bindValue(5, detectionLevelToString(r.level));
    q.bindValue(6, r.algoRoiRect.x());
    q.bindValue(7, r.algoRoiRect.y());
    q.bindValue(8, r.algoRoiRect.width());
    q.bindValue(9, r.algoRoiRect.height());
    q.bindValue(10, detectionModeToString(r.mode));
    q.bindValue(11, dl ? QVariant(r.anomalyScore) : QVariant());
    q.bindValue(12, dl ? QVariant(r.inferenceTimeMs) : QVariant());
    q.bindValue(13, r.timestampMs);
    q.bindValue(14, r.heatmapPath);
    q.bindValue(15, qMax(0, r.heatmapSize.width()));
    q.bindValue(16, qMax(0, r.heatmapSize.height()));
    if (!prepared || !q.exec())
        reportError(QStringLiteral("SQLite 写入失败，已请求停止检测；CSV 与数据库可能不一致。"), q.lastError().text());
}

QList<AlgoResult> ResultLogger::queryByLevel(DetectionLevel level, QString& error) const
{
    error.clear();
    QList<AlgoResult> list;
    if (!m_db.isOpen())
    {
        error = QStringLiteral("数据库未打开，无法查询历史记录。");
        return list;
    }
    QSqlQuery q(m_db);
    const bool prepared = q.prepare(
        "SELECT ts,frame,fi,p95,hotRatio,roi_x,roi_y,roi_w,roi_h,mode,score,"
        "inference_ms,frame_ts,heatmap_path,heatmap_w,heatmap_h "
        "FROM records WHERE level = ? ORDER BY ts DESC, rowid DESC LIMIT 1000");
    q.bindValue(0, detectionLevelToString(level));
    if (!prepared || !q.exec())
    {
        Logger::get()->error("History query failed: {}", q.lastError().text().toUtf8().toStdString());
        error = QStringLiteral("历史查询失败。\n技术详情：%1").arg(q.lastError().text());
        return list;
    }
    while (q.next())
    {
        AlgoResult r;
        r.recordedAtMs = QDateTime::fromString(q.value(0).toString(), Qt::ISODate).toMSecsSinceEpoch();
        r.frameIdx = q.value(1).toLongLong();
        r.fi = q.value(2).toDouble();
        r.p95 = q.value(3).toDouble();
        r.hotRatio = q.value(4).toDouble();
        r.algoRoiRect = QRect(q.value(5).toInt(), q.value(6).toInt(), q.value(7).toInt(), q.value(8).toInt());
        const QString mode = q.value(9).toString();
        r.hasModeInfo = mode == "Traditional" || mode == "DeepLearning";
        r.mode = mode == "DeepLearning" ? DetectionMode::DeepLearning : DetectionMode::Traditional;
        r.anomalyScore = q.value(10).toDouble();
        r.inferenceTimeMs = q.value(11).toLongLong();
        r.timestampMs = q.value(12).toLongLong();
        r.heatmapPath = q.value(13).toString();
        r.heatmapSize = QSize(q.value(14).toInt(), q.value(15).toInt());
        r.level = level;
        list.append(r);
    }
    return list;
}

bool ResultLogger::close()
{
    if (m_csvFile.isOpen())
    {
        flushCsv();
        m_csvStream.setDevice(nullptr);
        m_csvFile.close();
    }
    if (!m_connectionName.isEmpty())
    {
        m_db.close();
        m_db = QSqlDatabase(); // 先释放句柄，再移除 Qt 登记的连接
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
    }
    return !m_failed.load();
}
