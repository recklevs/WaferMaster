#pragma once

#include <memory>
#include <mutex>
#include <QString>
#include <QObject>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

// QtLogBridge — QObject，主线程驻留，接收日志信号
class QtLogBridge : public QObject
{
    Q_OBJECT
public:
    explicit QtLogBridge(QObject* parent = nullptr) : QObject(parent) {}
signals:
    void logMessage(const QString& message);
};

// QtLogSink — spdlog sink，工作线程格式化消息后通过 QueuedConnection 投递到 QtLogBridge
class QtLogSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
    explicit QtLogSink(QtLogBridge* bridge) : m_bridge(bridge) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;  // 关键：格式化 + 跨线程信号投递
    void flush_() override {}

private:
    QtLogBridge* m_bridge = nullptr;
};

// Logger — 全局日志单例，管理 spdlog::logger（控制台 + 文件 + Qt UI 三 sink）
class Logger
{
public:
    static void init(const QString& logDir = QStringLiteral("logs"));  // 创建各 sink 并组装
    static std::shared_ptr<spdlog::logger> get();
    static QtLogBridge* getBridge();  // 给 MainWindow::setupConnections() 连接信号

private:
    static std::shared_ptr<spdlog::logger> s_logger;
    static QtLogBridge*                    s_bridge;
};
