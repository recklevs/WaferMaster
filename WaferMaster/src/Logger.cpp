#include "Logger.h"
#include <QMetaObject>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

// 格式化日志文本，通过 QueuedConnection 安全投递到主线程的 QtLogBridge
void QtLogSink::sink_it_(const spdlog::details::log_msg& msg)
{
    spdlog::memory_buf_t formatted;
    base_sink<std::mutex>::formatter_->format(msg, formatted);
    QString text = QString::fromUtf8(
        formatted.data(), static_cast<int>(formatted.size())).trimmed();
    QMetaObject::invokeMethod(m_bridge, [bridge = m_bridge, text]() {
        emit bridge->logMessage(text);
    }, Qt::QueuedConnection);
}

std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;
QtLogBridge*                    Logger::s_bridge  = nullptr;

void Logger::init(const QString& logDir)
{
    // 1. 主线程创建 QtLogBridge（QObject 必须属于主线程）
    s_bridge = new QtLogBridge();

    // 2. 控制台 sink（debug 级别，彩色输出）
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(spdlog::level::debug);

    // 3. 文件 sink（info 级别，单文件最大 5MB，保留 7 个滚动文件）
    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        (logDir + QStringLiteral("/wafermaster.log")).toStdString(),
        5 * 1024 * 1024, 7);
    fileSink->set_level(spdlog::level::info);

    // 4. Qt UI sink（info 级别，投递到 MainWindow 的 log 窗口）
    auto qtSink = std::make_shared<QtLogSink>(s_bridge);
    qtSink->set_level(spdlog::level::info);

    // 5. 组装三路 sink 的 logger
    s_logger = std::make_shared<spdlog::logger>(
        "WaferMaster",
        spdlog::sinks_init_list{consoleSink, fileSink, qtSink});
    s_logger->set_level(spdlog::level::debug);
    s_logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
}

std::shared_ptr<spdlog::logger> Logger::get()  { return s_logger; }
QtLogBridge*                    Logger::getBridge() { return s_bridge; }