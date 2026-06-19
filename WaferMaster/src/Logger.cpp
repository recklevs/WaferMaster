#include "Logger.h"
#include <QMetaObject>
#include <spdlog/sinks/stdout_color_sinks.h>//彩色控制台输出
#include <spdlog/sinks/rotating_file_sink.h>//滚动文件输出

std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;//非const 静态成员不能在类内初始化
QtLogBridge*                    Logger::s_bridge  = nullptr;//启动时不指向任何地址，Logger::init() 中创建并赋值

// 格式化日志文本，通过 QueuedConnection 安全投递到主线程的 QtLogBridge
void QtLogSink::sink_it_(const spdlog::details::log_msg& msg)
{
    spdlog::memory_buf_t formatted;//准备一个内存缓冲区来存储格式化后的日志文本
    base_sink<std::mutex>::formatter_->format(msg, formatted);//调用父类 base_sink 的 formatter_ 成员格式化日志消息 msg，结果存储在 formatted 中

    QString text = QString::fromUtf8(
        formatted.data(), static_cast<int>(formatted.size())).trimmed();
    QMetaObject::invokeMethod(m_bridge, [bridge = m_bridge, text]() //使用 Qt 的元对象系统在 m_bridge 所在线程（主线程）中调用一个 lambda 函数，参数通过值捕获传入
    {
        emit bridge->logMessage(text);
    }, Qt::QueuedConnection);//排队异步调用
}
void Logger::init(const QString& logDir)
{
    // 1. 主线程创建 QtLogBridge（因为 `Logger::init()` 在 `main()` 里调用）
    s_bridge = new QtLogBridge();//父对象为 nullptr，生命周期由 Logger 管理，程序结束时自动销毁

    // 2. 控制台 sink（debug 级别，彩色输出）
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(spdlog::level::debug);//输出级别设置为 debug，即输出debug/info/warn/error/critical 

    // 3. 文件 sink（info 级别，单文件最大 5MB，保留 7 个滚动文件）
    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        (logDir + QStringLiteral("/wafermaster.log")).toStdString(),
        5 * 1024 * 1024, 7);
    fileSink->set_level(spdlog::level::info);

    // 4. Qt UI sink（info 级别，使用信号槽投递到 MainWindow 的 log 窗口）
    auto qtSink = std::make_shared<QtLogSink>(s_bridge);
    qtSink->set_level(spdlog::level::info);

    // 5. 组装三路 sink 的 logger
    s_logger = std::make_shared<spdlog::logger>(
        "WaferMaster",
        spdlog::sinks_init_list{consoleSink, fileSink, qtSink});
    s_logger->set_level(spdlog::level::debug);
    s_logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");//日志格式：时间戳 + 级别（彩色）+ 消息内容
}

std::shared_ptr<spdlog::logger> Logger::get()  { return s_logger; }
QtLogBridge*                    Logger::getBridge() { return s_bridge; }