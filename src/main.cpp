#include "stdafx.h"
#include "main.hpp"

#include "core/solAppSettings.hpp"
#include "core/solLanguageManager.hpp"
#include "core/solQlementineTheme.hpp"
#include "core/solThemeManager.hpp"
#include "utils/solBackgroundWork.hpp"
#include "utils/solPhaseTrace.hpp"
#include "MainWindow.hpp"

#include <QThreadPool>

#include <cstdlib>

namespace {

/// 협조적 취소가 걸린 배경 작업이 스스로 멈추기를 기다리는 상한.
/// 취소를 확인하는 지점이 루프 한 바퀴 단위라 그보다 오래 걸릴 이유가 없다.
/// 넘겨도 ~QCoreApplication 이 결국 기다리므로, 이 값은 해결책이 아니라
/// **경계선**이다 — 실제 해결은 각 루프의 isShuttingDown() 검사다.
constexpr int kBackgroundDrainMs = 3000;

}  // namespace

// CMake 가 생성한다. app.rc 의 VERSIONINFO 와 같은 값을 쓴다.
#include "mrst_version.h"

int main( int argc, char* argv[] )
{
    mrst::startPhaseClock();
    mrst::traceP( "main.enter" );
    // 프로세스가 실제로 빠져나간 시각. ~QCoreApplication 이 전역 스레드풀을
    // **무제한**으로 기다리므로(qtbase 의 qcoreapplication.cpp), "창은 사라졌는데
    // 프로세스가 남는" 구간은 여기서만 관측된다.
    std::atexit( [] { mrst::traceP( "atexit" ); } );

    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox");

    int exitCode = 0;

    // QApplication 을 스코프로 감싸는 이유는 계측 하나뿐이다. ~QApplication 이
    // 가장 먼저 qt_call_post_routines() 로 Chromium 을 파괴하고, 이어서
    // ~QCoreApplication 이 전역 스레드풀을 기다린다. 그 구간의 길이를 재려면
    // 파괴가 return 보다 앞에 와야 한다.
    {
        QApplication app( argc, argv );
        mrst::traceP( "qapp.ready" );
        QApplication::setApplicationName( QStringLiteral( MRST_PRODUCT_NAME ) );
        QApplication::setOrganizationName( "myHouse" );
        // 파일 속성의 버전과 어긋나지 않도록 리소스와 같은 값을 쓴다.
        QApplication::setApplicationVersion( QStringLiteral( MRST_VERSION_STRING ) );
        // 창 / 작업 표시줄 / Alt+Tab 아이콘. 탐색기가 보여 주는 .exe 아이콘은
        // 이것과 별개로 resources/app.rc 의 ICON 리소스가 담당한다.
        QApplication::setWindowIcon( QIcon( QStringLiteral( ":/icons/app.png" ) ) );

        // 0.4.0 에서 실행 파일 이름과 함께 설정 파일 이름도 바뀌었다. 구 파일을
        // 새 이름으로 한 번 옮긴다. **설정을 처음 읽는 곳(바로 아래 번역기 설치)
        // 보다 앞이어야 한다** — 뒤에 두면 첫 실행이 기본값으로 굳고, 그 뒤 저장이
        // 일어나면 새 ini 가 만들어져 마이그레이션이 영영 일어나지 않는다.
        if( AppSettings::migrateLegacyFile() )
            qInfo( "이전 버전의 설정을 옮겼습니다: %s", qPrintable( AppSettings::iniFilePath() ) );

        // 번역기를 테마·창보다 **먼저** 건다. MainWindow 생성자가 이미 tr() 을 수십
        // 번 부르므로, 번역기가 그보다 늦게 붙으면 LanguageChange 를 받을 위젯이
        // 아직 없어서 첫 화면만 원문(한국어)으로 굳는다.
        //
        // AppSettings 가 QApplication::applicationDirPath() 를 쓰므로 QApplication
        // 생성 뒤여야 한다. 번역기 자체는 싱글톤이 값으로 들고 있다 — 아래
        // do{}while(false) 블록 안에 스택 객체로 두면 블록을 벗어나는 순간 파괴된다.
        LanguageManager::installAtStartup();

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
            const ThemeManager::Theme savedTheme = ThemeManager::savedTheme();

            // QStyle 은 위젯이 하나도 없을 때 갈아끼워야 한다.
            // 테마 리소스를 못 읽는 등으로 실패하면 예전처럼 Fusion 으로 돌아간다.
            if( !QlementineTheme::install( savedTheme ) )
            {
                qWarning( "Qlementine 스타일 설치 실패 (:/themes/*.json 확인). Fusion 으로 대체합니다." );
                QApplication::setStyle( QStringLiteral( "Fusion" ) );
            }

            ThemeManager::instance().setTheme( savedTheme );
            ThemeManager::instance().applyToApplication();
            mrst::traceP( "theme.ready" );

            MainWindow* window = new MainWindow;

            // 명령줄로 받은 폴더/파일들. 실제로 여는 것은 창이 처음 그려진 뒤다.
            // 예) "MultiRoot-reST Editor.exe" D:\Docs D:\Docs\A\index.rst D:\Docs\B\index.rst
            //
            // 여기서 곧바로 열면 안 된다: show() 는 페인트를 **예약만** 하고 첫
            // 프레임은 아래 exec() 가 이벤트 루프를 돌려야 나온다. 그래서 show() 와
            // exec() 사이에 무엇을 두든 그 시간이 통째로 "창이 비어 있는 시간" 이
            // 된다(탭 복원 + Chromium 부팅으로 실측 1.3초 이상이었다).
            QStringList arguments = QApplication::arguments();
            arguments.removeFirst();   // 실행 파일 경로
            window->setStartupPaths( arguments );

            window->show();

        } while( false );

        exitCode = QApplication::exec();
        mrst::traceP( "main.exec-returned" );

        // ~QCoreApplication 이 전역 스레드풀을 **무제한**으로 기다린다
        // (qtbase 의 qcoreapplication.cpp). 그것이 "창은 사라졌는데 프로세스가
        // 남는" 구간의 정체다. 여기서 상한을 두어 그 구간을 끊는다.
        //
        // 이 상한이 안전한 것은 **저장이 별도 풀로 분리된 뒤부터**다.
        // MainWindow::saveView() 는 저장이 "시작" 되었다는 뜻으로 true 를
        // 돌려주고 그 직후 shutdownUi() 가 뷰를 지운다. 즉 지금까지 저장된
        // 파일이 온전했던 것은 저 무제한 대기 덕이었다. persistencePool() 은
        // 아래에서 끝까지 기다린다.
        QThreadPool::globalInstance()->clear();
        if( !QThreadPool::globalInstance()->waitForDone( kBackgroundDrainMs ) )
            qWarning( "배경 작업이 제한 시간 안에 끝나지 않았습니다." );
        mrst::traceP( "main.pool-drained" );

        // 저장과 hot-exit 스냅샷은 끝까지 기다린다. 데이터다.
        mrst::persistencePool().waitForDone();
        mrst::traceP( "main.persistence-drained" );
    }
    mrst::traceP( "main.app-destroyed" );

    return exitCode;
}
