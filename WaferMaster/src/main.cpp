#include <QApplication>
#include "MainWindow.h"
#include "Common.h"

int main(int argc, char *argv[])
{
    qRegisterMetaType<AlgoResult>("AlgoResult");

    QApplication app(argc, argv);

    MainWindow window;
    window.show();

    return app.exec();
}
