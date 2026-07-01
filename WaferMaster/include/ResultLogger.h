#pragma once
#include "Common.h"
#include <QObject>
#include <QFile>
#include <QTextStream>
#include <QList>
#include <QtSql/QSqlDatabase>

class ResultLogger : public QObject
{
    Q_OBJECT
public:
    explicit ResultLogger(QObject* parent = nullptr);
    ~ResultLogger();

    void setMode(StorageMode mode,
                 const QString& csvPath = QStringLiteral("records.csv"),
                 const QString& dbPath  = QStringLiteral("records.db"));

    StorageMode mode() const { return m_mode; }

    /// 每帧算法完成后调用，按当前模式写入对应存储
    void append(const AlgoResult& result);

    /// 查询接口：按检测等级筛选历史记录（面试演示用）
    QList<AlgoResult> queryByLevel(DetectionLevel level) const;

signals:
    void logMessage(const QString& msg); // 接入主窗口日志面板

private:
    bool initCsv(const QString& path);
    bool initSqlite(const QString& path);
    void appendCsv(const AlgoResult& r);
    void appendSqlite(const AlgoResult& r);

    StorageMode  m_mode = StorageMode::None;
    QFile        m_csvFile;
    QTextStream  m_csvStream;
    QSqlDatabase m_db;
};