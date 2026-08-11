#include "stdafx.h"
#include "main.hpp"

#include "core/solAppSettings.hpp"
#include "core/solQlementineTheme.hpp"
#include "core/solThemeManager.hpp"
#include "MainWindow.hpp"

// CMake 가 생성한다. app.rc 의 VERSIONINFO 와 같은 값을 쓴다.
#include "mrst_version.h"

int main( int argc, char* argv[] )
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox");
    QApplication app( argc, argv );
    QApplication::setApplicationName( "MultiRoot reST Editor" );
    QApplication::setOrganizationName( "myHouse" );
    // 파일 속성의 버전과 어긋나지 않도록 리소스와 같은 값을 쓴다.
    QApplication::setApplicationVersion( QStringLiteral( MRST_VERSION_STRING ) );
    // 창 / 작업 표시줄 / Alt+Tab 아이콘. 탐색기가 보여 주는 .exe 아이콘은
    // 이것과 별개로 resources/app.rc 의 ICON 리소스가 담당한다.
    QApplication::setWindowIcon( QIcon( QStringLiteral( ":/icons/app.png" ) ) );

    do
    {
        // 저장된 테마 설정을 읽기만 한다 (기본값은 다크).
        //
        // 여기서 곧바로 setTheme() 을 부르면 안 된다. 그 시점에는 QlementineTheme 이
        // 아직 설치되지 않아 applyToApplication() 이 레거시 전역 스타일시트를 씌우고,
        // 뒤이은 QApplication::setStyle() 이 QlementineStyle 을 QStyleSheetStyle 로
        // 감싸 버린다. 그러면 스타일시트를 다시는 벗지 못하고, 스타일시트가 살아 있는
        // 한 Qt 는 위젯을 **생성 도중에** polish 하므로 qlementine v1.4.2 의
        // ComboboxItemViewFilter 재진입 버그로 첫 QComboBox 에서 스택 오버플로가 난다.
        // (기본값이 다크라 다크에서는 setTheme 이 조기 반환해 증상이 없었다.)
        AppSettings settings;
        const auto savedTheme = static_cast< ThemeManager::Theme >(
            settings.value( "theme", static_cast< int >( ThemeManager::Dark ) ).toInt() );

        // QStyle 은 위젯이 하나도 없을 때 갈아끼워야 한다.
        // 테마 리소스를 못 읽는 등으로 실패하면 예전처럼 Fusion 으로 돌아간다.
        if( !QlementineTheme::install( savedTheme ) )
        {
            qWarning( "Qlementine 스타일 설치 실패 (:/themes/*.json 확인). Fusion 으로 대체합니다." );
            QApplication::setStyle( QStringLiteral( "Fusion" ) );
        }

        ThemeManager::instance().setTheme( savedTheme );
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