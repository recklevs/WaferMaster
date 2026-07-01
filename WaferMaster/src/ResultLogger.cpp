#include "ResultLogger.h"
#include "Logger.h"
#include <QDateTime>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

ResultLogger::ResultLogger(QObject* parent)
    : QObject(parent)
{}

ResultLogger::~ResultLogger()
{
    if (m_db.isOpen())
        m_db.close();
}

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

bool ResultLogger::initCsv(const QString& path)
{
    m_csvFile.setFileName(path);
    const bool existed = m_csvFile.exists();

    if (!m_csvFile.open(QIODevice::Append | QIODevice::Text))
    {
        Logger::get()->error("ResultLogger: cannot open CSV: {}", path.toStdString());
        return false;
    }

    m_csvStream.setDevice(&m_csvFile);

    if (!existed)
    {
        m_csvStream << "ts,frame,fi,p95,hotRatio,level,roi_x,roi_y,roi_w,roi_h\n";
        m_csvStream.flush();
    }

    Logger::get()->info("ResultLogger: CSV ready: {}", path.toStdString());
    return true;
}

bool ResultLogger::initSqlite(const QString& path)
{
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ResultLoggerConn"));
    m_db.setDatabaseName(path);

    if (!m_db.open())
    {
        Logger::get()->error("ResultLogger: cannot open SQLite: {}",
                             m_db.lastError().text().toStdString());
        return false;
    }

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

void ResultLogger::append(const AlgoResult& result)
{
    // 运行在主线程（由 onAlgorithmResultReady 调用），无需加锁
    if (m_mode == StorageMode::Csv    || m_mode == StorageMode::Both)
        appendCsv(result);
    if (m_mode == StorageMode::Sqlite || m_mode == StorageMode::Both)
        appendSqlite(result);
}

void ResultLogger::appendCsv(const AlgoResult& r)
{
    if (!m_csvFile.isOpen()) return;

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

    m_csvStream.flush();
}

void ResultLogger::appendSqlite(const AlgoResult& r)
{
    if (!m_db.isOpen()) return;

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
        Logger::get()->warn("ResultLogger: SQLite insert failed: {}",
                            q.lastError().text().toStdString());
    }
}

QList<AlgoResult> ResultLogger::queryByLevel(DetectionLevel level) const
{
    QList<AlgoResult> list;
    if (!m_db.isOpen()) return list;

    QSqlQuery q(m_db);
    q.prepare(
        "SELECT fi, p95, hotRatio, roi_x, roi_y, roi_w, roi_h "
        "FROM records WHERE level = ? ORDER BY ts DESC");
    q.bindValue(0, detectionLevelToString(level));

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
        r.frameIdx = -1; // 标识为历史查询记录
        list.append(r);
    }
    return list;
}