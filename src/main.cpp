#include "stdafx.h"
#include "main.hpp"
#include "MainWindow.hpp"

int main( int argc, char* argv[] )
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox");
    QApplication app( argc, argv );
    QApplication::setStyle( QStringLiteral( "Fusion" ) );
    MainWindow window;

    do
    {
        window.show();

    } while( false );

    return QApplication::exec();
}