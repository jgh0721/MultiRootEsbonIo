#include "stdafx.h"
#include "main.hpp"

#include "core/solAppSettings.hpp"
#include "core/solThemeManager.hpp"
#include "MainWindow.hpp"

int main( int argc, char* argv[] )
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox");
    QApplication app( argc, argv );
    QApplication::setStyle( QStringLiteral( "Fusion" ) );
    QApplication::setApplicationName( "MultiRoot reST Editor" );
    QApplication::setOrganizationName( "myHouse" );
    QApplication::setApplicationVersion( "0.0.1" );

    do
    {
        // 저장된 테마 설정 복원 후 적용
        {
            AppSettings settings;
            const int savedTheme = settings.value("theme", static_cast<int>(ThemeManager::Light)).toInt();
            ThemeManager::instance().setTheme(static_cast<ThemeManager::Theme>(savedTheme));
        }
        ThemeManager::instance().applyToApplication();

        MainWindow* window = new MainWindow;
        window->show();

        // 명령줄로 받은 폴더/파일들을 연다.
        // 예) MultiRoot-reST-CPP.exe D:\Docs D:\Docs\A\index.rst D:\Docs\B\index.rst
        QStringList arguments = QApplication::arguments();
        arguments.removeFirst();   // 실행 파일 경로
        if( !arguments.isEmpty() )
            window->openStartupPaths( arguments );
        else
            window->restoreLastSession();

    } while( false );

    return QApplication::exec();
}