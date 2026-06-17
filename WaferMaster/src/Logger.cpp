#include "Common.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;

void Logger::init(const QString& logDir)
{
    // 同时输出到控制台（彩色）和滚动文件
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(spdlog::level::debug);

    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        (logDir + QStringLiteral("/wafermaster.log")).toStdString(),
        5 * 1024 * 1024,  // 单文件 5MB
        7                  // 最多保留 7 个文件
    );
    fileSink->set_level(spdlog::level::info);

    s_logger = std::make_shared<spdlog::logger>(
        "WaferMaster",
        spdlog::sinks_init_list{consoleSink, fileSink}
    );
    s_logger->set_level(spdlog::level::debug);
    s_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

    spdlog::register_logger(s_logger);
}

std::shared_ptr<spdlog::logger> Logger::get()
{
    return s_logger;
}