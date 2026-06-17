#include <QApplication>
#include <spdlog/spdlog.h>
#include "MainWindow.h"
#include "Common.h"

int main(int argc, char *argv[])
{
    // 日志系统最先初始化
    Logger::init();
    Logger::get()->info("WaferMaster started");

    qRegisterMetaType<AlgoResult>("AlgoResult");
//让 Qt 的元对象系统认识 `AlgoResult` 这个自定义类型，支持跨线程队列传递。
    QApplication app(argc, argv);

    MainWindow window;
    window.show();

    return app.exec();
}
