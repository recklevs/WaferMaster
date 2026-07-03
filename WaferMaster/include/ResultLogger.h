#pragma once

#include "Common.h"
#include <QObject>
#include <QFile>
#include <QTextStream>
#include <QList>
#include <QtSql/QSqlDatabase>

/** @brief 检测结果持久化存储层：将每一帧算法结果（AlgoResult）写入 CSV 和/或 SQLite，
           并支持按检测等级查询历史记录（面试演示用）

    ## 双写策略
     - CSV：人类可读，可用 Excel / 文本编辑器直接打开，适合快速查看
     - SQLite：结构化存储，支持 SQL 查询（如按检测等级筛选），适合数据分析

    ## 工作流程
     1. setMode() → 根据 StorageMode 初始化 CSV 文件 / SQLite 数据库
     2. append() → 每帧算法完成后由 MainWindow 调用，写入当前结果
     3. queryByLevel() → 从 SQLite 按检测等级筛选历史记录（仅 SQLite 模式可用）
     4. ~ResultLogger() → 关闭 SQLite 连接，CSV 由 QFile 析构自动关闭

    ## 线程安全
     本模块运行在主线程（通过 MainWindow::onAlgorithmResultReady 信号槽触发），
     无需额外加锁。
*/
class ResultLogger : public QObject
{
    Q_OBJECT

public:
    /// @brief 构造结果日志记录器
    /// @param parent 父 QObject（通常为 MainWindow）
    explicit ResultLogger(QObject* parent = nullptr);

    /// @brief 析构，关闭 SQLite 数据库连接（CSV 由 QFile 自动关闭）
    ~ResultLogger();

    /// @brief 设置存储模式和输出路径，按需初始化 CSV / SQLite
    /// @param mode    存储模式：None / Csv / Sqlite / Both
    /// @param csvPath CSV 文件完整路径
    /// @param dbPath  SQLite 数据库文件完整路径
    /// @note 路径应由 MainWindow::initResultLogger 统一推导后传入
    void setMode(StorageMode mode,
                 const QString& csvPath,const QString& dbPath);

    /// @brief 返回当前存储模式
    StorageMode mode() const { return m_mode; }

    /// @brief 每帧算法完成后调用，按当前模式将结果写入对应存储后端
    /// @param result 算法检测结果（包含帧号、算法指标、ROI 等）
    /// @note 主线程调用，无需加锁
    void append(const AlgoResult& result);

    /// @brief 按检测等级从 SQLite 查询历史记录
    /// @param level 检测等级（Normal / Warning / NG）
    /// @return 符合条件的历史记录列表（按时间倒序），无结果时返回空列表
    /// @note 仅在 StorageMode::Sqlite 或 Both 模式且数据表存在时有效
    QList<AlgoResult> queryByLevel(DetectionLevel level) const;

signals:
    // ========================================================================
    // 日志信号 → MainWindow 日志面板
    // ========================================================================

    /// @brief 存储层日志（CSV/SQLite 初始化结果），转发到主窗口日志控件
    /// @param msg 日志文本
    void logMessage(const QString& msg);

private:
    // ========================================================================
    // 内部初始化函数
    // ========================================================================

    /// @brief 初始化 CSV 文件（Append 模式，首次写入表头）
    /// @param path CSV 文件完整路径
    /// @return true=初始化成功，false=文件无法打开
    bool initCsv(const QString& path);

    /// @brief 初始化 SQLite 数据库（CREATE TABLE IF NOT EXISTS）
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

    // ========================================================================
    // 成员变量
    // ========================================================================

    StorageMode  m_mode = StorageMode::None; ///< 当前存储模式
    QFile        m_csvFile;                   ///< CSV 文件句柄
    QTextStream  m_csvStream;                 ///< CSV 写入流（绑定 m_csvFile）
    QSqlDatabase m_db;                        ///< SQLite 数据库连接
};