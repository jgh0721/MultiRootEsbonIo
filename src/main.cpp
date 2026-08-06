#include "stdafx.h"
#include "main.hpp"

#include "core/solAppSettings.hpp"
#include "core/solQlementineTheme.hpp"
#include "core/solThemeManager.hpp"
#include "MainWindow.hpp"

int main( int argc, char* argv[] )
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox");
    QApplication app( argc, argv );
    QApplication::setApplicationName( "MultiRoot reST Editor" );
    QApplication::setOrganizationName( "myHouse" );
    QApplication::setApplicationVersion( "0.0.1" );

    do
    {
        // 저장된 테마 설정 복원 (기본값은 다크)
        {
            AppSettings settings;
            const int savedTheme = settings.value("theme", static_cast<int>(ThemeManager::Dark)).toInt();
            ThemeManager::instance().setTheme(static_cast<ThemeManager::Theme>(savedTheme));
        }

        // QStyle 은 위젯이 하나도 없을 때 갈아끼워야 한다.
        // 테마 리소스를 못 읽는 등으로 실패하면 예전처럼 Fusion 으로 돌아간다.
        if( !QlementineTheme::install() )
        {
            qWarning( "Qlementine 스타일 설치 실패 (:/themes/*.json 확인). Fusion 으로 대체합니다." );
            QApplication::setStyle( QStringLiteral( "Fusion" ) );
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