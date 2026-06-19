#pragma once

#include <memory>
#include <mutex>
#include <QString>
#include <QObject>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

// ============================================================================
// QtLogBridge — QObject 桥接器，驻留在主线程，接收来自工作线程的日志信号
// ============================================================================

/** @brief spdlog → Qt UI 的桥梁：作为 QObject 驻留在主线程，
           工作线程通过 QtLogSink 以 QueuedConnection 向其投递日志消息
*/
class QtLogBridge : public QObject
{
    Q_OBJECT
public:
    /// @brief 构造桥接器，/// parent = nullptr：不由父 QObject 自动析构，生命周期由 Logger::s_bridge 手动管理
    explicit QtLogBridge(QObject* parent = nullptr) : QObject(parent) {}

signals:
    /// @brief 收到一条格式化后的日志消息，由 MainWindow 连接以追加到日志控件
    /// @param message 格式化后的日志文本（含时间戳、级别、内容）
    void logMessage(const QString& message);
};

// ============================================================================
// QtLogSink — spdlog sink 实现，将日志消息从任意线程投递到 QtLogBridge
// ============================================================================

/** @brief 自定义 spdlog sink：在 sink_it_() 中格式化日志消息，
           通过 QMetaObject::invokeMethod（QueuedConnection）跨线程投递到 QtLogBridge
*/
class QtLogSink : public spdlog::sinks::base_sink<std::mutex>//继承自spdlog的日志输出目标模板，使用 std::mutex 保护线程安全
{
public:
    /// @brief 构造 sink，绑定到指定 QtLogBridge
    /// @param bridge 目标桥接器指针（由 Logger::init() 创建并传入）
    explicit QtLogSink(QtLogBridge* bridge) : m_bridge(bridge) {}//创建 QtLogSink 对象时接收一个 QtLogBridge* 指针，把它保存到成员变量 m_bridge 中

protected:
    /// @brief spdlog 回调：收到一条日志消息时调用
    ///        内部格式化后通过 QueuedConnection 投递到 m_bridge
    /// @param msg spdlog 日志消息结构体（含级别、时间戳、负载文本）
    void sink_it_(const spdlog::details::log_msg& msg) override;//重写 base_sink 的纯虚函数 sink_it_()，当 spdlog 需要输出一条日志时会调用这个函数
     //spdlog::details::log_msg 是 spdlog 内部定义的一个结构体，包含了日志消息的各种信息
    /// @brief spdlog 回调：（本实现为空，不需要刷新，但父类是 = 0（纯虚），不写编译不过）
    void flush_() override {}

private:
    QtLogBridge* m_bridge = nullptr;  //绑定的 QtLogBridge 实例
};

// ============================================================================
// Logger — 全局日志单例，管理 spdlog::logger（控制台 + 文件 + Qt UI 三 sink）
// ============================================================================

/** @brief 全局日志单例：封装 spdlog，同时输出到控制台、滚动日志文件和 Qt UI

    ## 三 sink 架构
     - Console sink：带颜色，开发调试用
     - Rotating file sink：按大小滚动（默认 5MB × 3 个文件），持久化存储
     - Qt UI sink：通过 QtLogSink → QtLogBridge 跨线程投递到 MainWindow 日志控件
*/
class Logger//全员static，不需实例化，全局单例
{
public:
    /// @brief 初始化全局日志器（程序启动时调用一次）
    ///        创建控制台 sink、文件 sink、Qt UI sink 并组装到 spdlog::logger
    /// @param logDir 日志文件存放目录（若不传参默认保存logs目录）
    static void init(const QString& logDir = QStringLiteral("logs"));

    /// @brief 获取全局 spdlog::logger 实例
    /// @return 返回共享指针，任何线程均可安全调用自动销毁
    static std::shared_ptr<spdlog::logger> get();

    /// @brief 获取 QtLogBridge 指针，供 MainWindow::setupConnections() 连接信号
    /// @return 桥接器指针（init() 后有效，否则为 nullptr）
    static QtLogBridge* getBridge();

private:
    static std::shared_ptr<spdlog::logger> s_logger;  ///< spdlog::logger 全局单例指针
    static QtLogBridge*                    s_bridge;  ///< QtLogBridge 全局单例指针
};