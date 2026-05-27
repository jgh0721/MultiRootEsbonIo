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

        MainWindow window;
        window.show();

    } while( false );

    return QApplication::exec();
}