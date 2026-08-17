#include "solUpdaterStrings.hpp"
#include "solUpdaterSwap.hpp"

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <string>
#include <vector>

/// MultiRoot reST 업데이터.
///
/// 앱이 자기 자신을 교체할 수 없기 때문에 존재한다. 실행 중인 프로세스는 자기
/// exe 와 로드된 DLL 을 지울 수 없고, 교체가 중간에 실패했을 때 되돌릴 주체도
/// 필요하다. 그래서 앱은 이 프로그램을 %TEMP% 사본으로 띄우고 곧바로 종료한다.
///
/// **Qt 를 링크하지 않는다.** Qt6Core.dll 을 로드하면 자기가 교체해야 하는
/// 파일을 스스로 잠근다. 창도 띄우지 않는다(WIN32 서브시스템이라 콘솔도 없다) —
/// 결과는 --log 파일과 --result INI 로 남기고, 앱이 다음 기동에서 읽어 보고한다.
/// 유일한 예외는 사용자가 직접 실행하는 --rollback 이다.
///
/// 사용법:
///   mrst_updater.exe --pid <앱 pid> --target <설치폴더> --staging <준비폴더>
///                    --backup <백업폴더> --log <로그경로> --result <결과경로>
///                    --version <버전> [--remove <이름>]... [--relaunch <앱exe>]
///   mrst_updater.exe --rollback --target <설치폴더> --backup <백업폴더> --log <로그경로>

namespace {

using namespace mrst::updater;

/// 앱이 종료되기를 기다리는 시간.
///
/// 넉넉해야 한다. 앱의 종료 경로는 esbonio 프로세스마다 terminate(1.5초) +
/// kill(1.5초) 를 기다리고 기본값이 3개라 최악 9초가 걸린다. 프리뷰 빌드가
/// 돌던 중이면 더 걸릴 수 있다.
constexpr DWORD kWaitTimeoutMs = 90 * 1000;

/// 프로세스가 사라진 직후에도 백신이나 탐색기가 그 파일 핸들을 아직 놓지 않은
/// 순간이 있다. 한 번 양보하고 시작한다.
constexpr DWORD kSettleDelayMs = 500;

constexpr int kExitOk         = 0;
constexpr int kExitBadArgs    = 2;
constexpr int kExitAppAlive   = 3;
constexpr int kExitSwapFailed = 4;

struct Options
{
    unsigned long long          pid = 0;
    std::wstring                target;
    std::wstring                staging;
    std::wstring                backup;
    std::wstring                logPath;
    std::wstring                resultPath;
    std::wstring                version;
    std::wstring                relaunchPath;
    std::vector< std::wstring > removals;
    bool                        rollback = false;
};

[[nodiscard]] bool parseOptions( Options& options )
{
    int count = 0;
    wchar_t** argv = ::CommandLineToArgvW( ::GetCommandLineW(), &count );
    if( argv == nullptr )
        return false;

    // 값을 요구하는 옵션은 다음 인자가 있어야 한다. 없으면 파싱 실패로 본다.
    const auto takeValue = [ argv, count ]( int& index, std::wstring& out ) -> bool {
        if( index + 1 >= count )
            return false;
        out = argv[ ++index ];
        return true;
    };

    bool ok = true;
    for( int index = 1; index < count && ok; ++index )
    {
        const std::wstring option = argv[ index ];

        if( option == L"--rollback" )
        {
            options.rollback = true;
        }
        else if( option == L"--pid" )
        {
            std::wstring value;
            ok = takeValue( index, value );
            if( ok )
                options.pid = ::wcstoull( value.c_str(), nullptr, 10 );
        }
        else if( option == L"--target" )   { ok = takeValue( index, options.target ); }
        else if( option == L"--staging" )  { ok = takeValue( index, options.staging ); }
        else if( option == L"--backup" )   { ok = takeValue( index, options.backup ); }
        else if( option == L"--log" )      { ok = takeValue( index, options.logPath ); }
        else if( option == L"--result" )   { ok = takeValue( index, options.resultPath ); }
        else if( option == L"--version" )  { ok = takeValue( index, options.version ); }
        else if( option == L"--relaunch" ) { ok = takeValue( index, options.relaunchPath ); }
        else if( option == L"--remove" )
        {
            std::wstring value;
            ok = takeValue( index, value );
            if( ok && !value.empty() )
                options.removals.push_back( value );
        }
        else
        {
            // 모르는 옵션은 무시한다. 새 앱이 옛 업데이터를 부르는 경우가 있고
            // (교체 직전의 업데이터는 항상 구버전이다) 그때 죽으면 안 된다.
        }
    }

    ::LocalFree( argv );

    if( options.target.empty() || options.backup.empty() )
        return false;
    if( !options.rollback && ( options.staging.empty() || options.pid == 0 ) )
        return false;
    return ok;
}

}  // namespace

int APIENTRY wWinMain( HINSTANCE, HINSTANCE, LPWSTR, int )
{
    Options options;
    if( !parseOptions( options ) )
    {
        // 인자는 앱이 만들어 준다. 여기로 오는 것은 사람이 직접 실행한 경우뿐이라
        // 사용법을 보여 주는 편이 낫다.
        const UiLang lang = detectUiLanguage();
        ::MessageBoxW( nullptr, text( Text::Usage, lang ), text( Text::Title, lang ),
                      MB_ICONINFORMATION | MB_OK );
        return kExitBadArgs;
    }

    Log log( options.logPath );

    if( options.rollback )
    {
        log.line( L"=== 되돌리기 시작: %s -> %s ===", options.backup.c_str(), options.target.c_str() );
        const bool   ok   = rollbackAll( options.backup, options.target, log );
        const UiLang lang = detectUiLanguage();
        ::MessageBoxW( nullptr, text( ok ? Text::RollbackOk : Text::RollbackFailed, lang ),
                      text( Text::Title, lang ),
                      MB_OK | ( ok ? MB_ICONINFORMATION : MB_ICONERROR ) );
        return ok ? kExitOk : kExitSwapFailed;
    }

    log.line( L"=== 업데이트 시작: %s (pid %llu 종료 대기) ===",
             options.version.c_str(), options.pid );

    if( !waitForProcessExit( options.pid, kWaitTimeoutMs, log ) )
    {
        // 교체를 시작하지 않았으므로 설치 폴더는 그대로다. staging 도 남겨 두어
        // 다음 실행에서 다시 시도할 수 있다.
        // 문장이 아니라 식별자를 남긴다. 앱이 자기 언어로 문장을 만든다.
        writeResult( options.resultPath, false, options.version, FailureId::kAppStillAlive );
        return kExitAppAlive;
    }

    ::Sleep( kSettleDelayMs );

    if( !swapAll( options.staging, options.target, options.backup, options.removals, log ) )
    {
        const std::wstring reason = log.failure().empty()
            ? std::wstring( FailureId::kSwapFailed )
            : log.failure();
        writeResult( options.resultPath, false, options.version, reason, log.failureDetail() );
        log.line( L"=== 업데이트 실패 ===" );
        return kExitSwapFailed;
    }

    writeResult( options.resultPath, true, options.version, {} );

    // backup 은 지우지 않는다. 새 버전이 한 번 정상 기동하면 앱이 스스로
    // 치운다(그 시점이 롤백을 포기하는 지점이다). 그때까지는 --rollback 으로
    // 되돌릴 수 있다.
    if( !options.relaunchPath.empty() )
        static_cast< void >( relaunch( options.relaunchPath, log ) );

    log.line( L"=== 업데이트 완료 ===" );
    return kExitOk;
}
