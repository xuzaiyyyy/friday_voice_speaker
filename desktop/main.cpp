#include "expressionwindow.h"
#include "desktopidle.h"
#include "desktopcontrol.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QCursor>
#include <QtGlobal>

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTouchEvents, true);

    const QByteArray platformEnv = qgetenv("QT_QPA_PLATFORM");
    if (platformEnv.startsWith("xcb"))
    {
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
        if (qgetenv("QT_XCB_GL_INTEGRATION").isEmpty())
        {
            qputenv("QT_XCB_GL_INTEGRATION", QByteArray("none"));
        }
    }

    QApplication a(argc, argv);
    QApplication::setStartDragDistance(24);
    QApplication::setDoubleClickInterval(350);
    if (qEnvironmentVariableIntValue("XIAOMAN_DESKTOP_SHOW_CURSOR") == 0)
    {
        QApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    }

    installDesktopIdleReturn();
    new DesktopControlPipe(&a);
    ExpressionWindow w;
    showDesktopTopLevel(&w);
    return a.exec();
}
