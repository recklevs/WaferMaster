#pragma once

#include "Common.h"
#include <QObject>
#include <QFile>
#include <QTextStream>
#include <QList>
#include <QtSql/QSqlDatabase>
#include <atomic>

/** @brief 检测结果持久化存储层：将每一帧算法结果（AlgoResult）写入 CSV 和/或 SQLite，
           并支持按检测等级查询历史记录。

    ## 双写策略
     - CSV：人类可读，可用 Excel / 文本编辑器直接打开，适合快速查看
     - SQLite：结构化存储，支持 SQL 查询（如按检测等级筛选），适合数据分析

    ## 工作流程
     1. 存储线程启动后 setMode() 初始化文件和数据库；
     2. 主线程 enqueue() 只复制必要数据并排队，不执行磁盘写入；
     3. 存储线程 append() 写入结果，queryByLevel() 查询历史；
     4. 退出前 close() 排在已接受任务之后，收尾完成后再结束线程。

    ## 线程安全
     只有 enqueue() 可以由主线程直接调用。
     文件、数据库和其余函数只在存储线程使用；原子计数限制积压记录数量。
*/
class ResultLogger : public QObject
{
    Q_OBJECT

public:
    /// @brief 构造结果日志记录器
    /// @param parent 移入存储线程时必须为 nullptr
    explicit ResultLogger(QObject* parent = nullptr);

    /// @brief 在存储线程内刷新 CSV、关闭并移除 SQLite 连接
    ~ResultLogger();

    /// @brief 设置存储模式和输出路径，按需初始化 CSV / SQLite
    /// @param mode    存储模式：None / Csv / Sqlite / Both
    /// @param csvPath CSV 文件完整路径
    /// @param dbPath  SQLite 数据库文件完整路径
    /// @note 在存储线程调用，路径由 MainWindow::initResultLogger 推导
    bool setMode(StorageMode mode,
                 const QString& csvPath,const QString& dbPath);

    /// @brief 返回当前存储模式（仅存储线程读取）
    StorageMode mode() const { return m_mode; }

    /// @brief 主线程提交保存任务；队列满时明确报错，不悄悄丢记录
    /// @return true=已接受并排队，不表示已经写入磁盘
    bool enqueue(const AlgoResult& result);
    static constexpr int MaxPendingRecords = 64;

    /// @brief 在存储线程查询历史；主窗口用 BlockingQueuedConnection 等待返回
    /// @param error 查询失败时填写错误信息，区分“没有记录”和“查询失败”
    QList<AlgoResult> queryByLevel(DetectionLevel level, QString& error) const;

    /// @brief 在存储线程刷新文件并移除数据库连接；退出时先排队调用再 quit/wait
    /// @return false 表示本轮出现过保存错误
    bool close();

signals:
    // ========================================================================
    // 日志信号 → MainWindow 日志面板
    // ========================================================================

    /// @brief 存储层日志（CSV/SQLite 初始化结果），转发到主窗口日志控件
    /// @param msg 日志文本
    void logMessage(const QString& msg);
    void errorOccurred(const QString& message);

private:
    // ========================================================================
    // 内部初始化函数
    // ========================================================================

    /// @brief 初始化新版 CSV（Append 模式）；旧 records.csv 由主窗口另行保留
    /// @param path CSV 文件完整路径
    /// @return true=初始化成功，false=文件无法打开
    bool initCsv(const QString& path);

    /// @brief 初始化 SQLite，只为旧表补上缺少的字段，不删除旧记录
    /// @param path 数据库文件完整路径（.db）
    /// @return true=初始化成功，false=数据库无法打开
    bool initSqlite(const QString& path);

    // ========================================================================
    // 内部写入函数
    // ========================================================================

    /// @brief 将一条 AlgoResult 追加写入 CSV 文件末尾
    /// @param r 检测结果
    void appendCsv(const AlgoResult& r);

    /// @brief 将一条 AlgoResult 通过参数化 INSERT 写入 SQLite
    /// @param r 检测结果
    void appendSqlite(const AlgoResult& r);

    // 以下函数只由存储线程执行。
    void append(AlgoResult result);
    bool saveHeatmap(AlgoResult& result);
    bool flushCsv();
    void reportError(const QString& message, const QString& detail);

    // ========================================================================
    // 成员变量
    // ========================================================================

    StorageMode  m_mode = StorageMode::None; ///< 当前存储模式
    QFile        m_csvFile;                   ///< CSV 文件句柄
    QTextStream  m_csvStream;                 ///< CSV 写入流（绑定 m_csvFile）
    QSqlDatabase m_db;                        ///< SQLite 数据库连接
    QString m_connectionName;                ///< 每个存储器使用独立连接名
    QString m_heatmapDir;
    std::atomic_int m_pending{0};             ///< 已接受但尚未处理完的记录数
    std::atomic_bool m_failed{false};         ///< 保存失败后不再继续写半成品
};
