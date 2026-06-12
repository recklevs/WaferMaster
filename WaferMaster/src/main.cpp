#include <QApplication>
#include "MainWindow.h"
#include "Common.h"

int main(int argc, char *argv[])
{
    qRegisterMetaType<AlgoResult>("AlgoResult");
//让 Qt 的元对象系统认识 `AlgoResult` 这个自定义类型，支持跨线程队列传递。
    QApplication app(argc, argv);

    MainWindow window;
    window.show();

    return app.exec();
}
