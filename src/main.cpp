#include "MainWindow.h"
#include <QApplication>
#include <QSurfaceFormat>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ATEM Emulator");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("CEMC");

    MainWindow w;
    w.show();

    return app.exec();
}
