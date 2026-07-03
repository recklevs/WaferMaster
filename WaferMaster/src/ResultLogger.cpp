#include "ResultLogger.h"
#include "Logger.h"
#include <QDateTime>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

// ═══════════════════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════════════════

ResultLogger::ResultLogger(QObject* parent)
    : QObject(parent)
{
}

ResultLogger::~ResultLogger()
{
    if (m_db.isOpen())
        m_db.close();
}

// ═══════════════════════════════════════════════════════════════════
// 模式设置
// ═══════════════════════════════════════════════════════════════════

void ResultLogger::setMode(StorageMode mode,
                           const QString& csvPath,
                           const QString& dbPath)
{
    m_mode = mode;

    if (mode == StorageMode::Csv || mode == StorageMode::Both)
        initCsv(csvPath);

    if (mode == StorageMode::Sqlite || mode == StorageMode::Both)
        initSqlite(dbPath);
}

// ═══════════════════════════════════════════════════════════════════
// 存储后端初始化：CSV
// ═══════════════════════════════════════════════════════════════════

bool ResultLogger::initCsv(const QString& path)
{
    m_csvFile.setFileName(path);
    const bool existed = m_csvFile.exists();// 检查文件是否已存在（决定要不要写表头）

    if (!m_csvFile.open(QIODevice::Append | QIODevice::Text))//尝试以追加文本模式打开
    {
        Logger::get()->error("ResultLogger: cannot open CSV: {}", path.toStdString());
        return false;
    }

    m_csvStream.setDevice(&m_csvFile);// 将写入流绑定到已打开的 QFile

    // 文件不存在 → 写入表头；已存在则追加数据即可
    if (!existed)
    {
        m_csvStream << "ts,frame,fi,p95,hotRatio,level,roi_x,roi_y,roi_w,roi_h\n";
        m_csvStream.flush();//强制将输出缓冲区中的所有数据立即写入底层设备
    }

    Logger::get()->info("ResultLogger: CSV ready: {}", path.toStdString());
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// 存储后端初始化：SQLite
// ═══════════════════════════════════════════════════════════════════

bool ResultLogger::initSqlite(const QString& path)
{
    // 驱动类型：SQLite，使用独立连接名，避免与其他 QSqlDatabase 冲突
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ResultLoggerConn"));
    m_db.setDatabaseName(path);

    if (!m_db.open())
    {
        Logger::get()->error("ResultLogger: cannot open SQLite: {}",
                             m_db.lastError().text().toStdString());
        return false;
    }

    // CREATE TABLE IF NOT EXISTS — 幂等操作，表已存在时不会报错
    QSqlQuery q(m_db);
    q.exec(
        "CREATE TABLE IF NOT EXISTS records ("
        "ts TEXT, frame INTEGER, fi REAL, p95 REAL, "
        "hotRatio REAL, level TEXT, "
        "roi_x INTEGER, roi_y INTEGER, roi_w INTEGER, roi_h INTEGER)"
    );

    Logger::get()->info("ResultLogger: SQLite ready: {}", path.toStdString());
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// 写入入口（主线程调用）
// ═══════════════════════════════════════════════════════════════════

void ResultLogger::append(const AlgoResult& result)
{
    // 运行在主线程（由 MainWindow::onAlgorithmResultReady 调用），无需加锁
    if (m_mode == StorageMode::Csv    || m_mode == StorageMode::Both)
        appendCsv(result);
    if (m_mode == StorageMode::Sqlite || m_mode == StorageMode::Both)
        appendSqlite(result);
}

// ═══════════════════════════════════════════════════════════════════
// CSV 写入实现
// ═══════════════════════════════════════════════════════════════════

void ResultLogger::appendCsv(const AlgoResult& r)
{
    if (!m_csvFile.isOpen()) return;

    // 字段顺序必须与 initCsv 中的表头一致
    m_csvStream
        << QDateTime::currentDateTime().toString(Qt::ISODate) << ","
        << r.frameIdx << ","
        << r.fi << ","
        << r.p95 << ","
        << r.hotRatio << ","
        << detectionLevelToString(r.level) << ","
        << r.algoRoiRect.x() << ","
        << r.algoRoiRect.y() << ","
        << r.algoRoiRect.width() << ","
        << r.algoRoiRect.height() << "\n";

    // 立即刷新到磁盘，避免崩溃时丢失数据
    m_csvStream.flush();
}

// ═══════════════════════════════════════════════════════════════════
// SQLite 写入实现
// ═══════════════════════════════════════════════════════════════════

void ResultLogger::appendSqlite(const AlgoResult& r)
{
    if (!m_db.isOpen()) return;

    // 参数化查询，安全防 SQL 注入
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO records VALUES (?,?,?,?,?,?,?,?,?,?)");
    q.bindValue(0, QDateTime::currentDateTime().toString(Qt::ISODate));
    q.bindValue(1, r.frameIdx);
    q.bindValue(2, r.fi);
    q.bindValue(3, r.p95);
    q.bindValue(4, r.hotRatio);
    q.bindValue(5, detectionLevelToString(r.level));
    q.bindValue(6, r.algoRoiRect.x());
    q.bindValue(7, r.algoRoiRect.y());
    q.bindValue(8, r.algoRoiRect.width());
    q.bindValue(9, r.algoRoiRect.height());

    if (!q.exec())
    {
        // 写入失败仅输出警告，不中断主流程
        Logger::get()->warn("ResultLogger: SQLite insert failed: {}",
                            q.lastError().text().toStdString());
    }
}

// ═══════════════════════════════════════════════════════════════════
// 历史查询（仅 SQLite）
// ═══════════════════════════════════════════════════════════════════

QList<AlgoResult> ResultLogger::queryByLevel(DetectionLevel level) const
{
    QList<AlgoResult> list;
    if (!m_db.isOpen()) return list;

    QSqlQuery q(m_db);
    q.prepare(
        "SELECT fi, p95, hotRatio, roi_x, roi_y, roi_w, roi_h "
        "FROM records WHERE level = ? ORDER BY ts DESC");// prepare()是预编译SQL，?是占位符
    q.bindValue(0, detectionLevelToString(level));// 把 ? 替换为level字符串

    if (!q.exec()) return list;

    while (q.next())
    {
        AlgoResult r;
        r.fi       = q.value(0).toDouble();
        r.p95      = q.value(1).toDouble();
        r.hotRatio = q.value(2).toDouble();
        r.algoRoiRect = QRect(q.value(3).toInt(), q.value(4).toInt(),
                              q.value(5).toInt(), q.value(6).toInt());
        r.level    = level;
        r.frameIdx = -1; // 标识为历史查询记录，区别于实时帧
        list.append(r);
    }
    return list;
}