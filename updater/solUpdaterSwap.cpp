#include "solUpdaterSwap.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace mrst::updater {

namespace {

/// 잠금이 풀리기를 기다리는 재시도 횟수와 간격.
/// 20 x 250ms = 최대 5초. 백신 스캔은 보통 그 안에 끝난다.
constexpr unsigned kMaxAttempts   = 20;
constexpr DWORD    kRetryDelayMs  = 250;

[[nodiscard]] std::string toUtf8( const std::wstring& text )
{
    if( text.empty() )
        return {};

    const int size = ::WideCharToMultiByte( CP_UTF8, 0, text.c_str(), static_cast< int >( text.size() ),
                                           nullptr, 0, nullptr, nullptr );
    if( size <= 0 )
        return {};

    std::string result( static_cast< size_t >( size ), '\0' );
    ::WideCharToMultiByte( CP_UTF8, 0, text.c_str(), static_cast< int >( text.size() ),
                          result.data(), size, nullptr, nullptr );
    return result;
}

void appendUtf8( const std::wstring& path, const std::string& text )
{
    if( path.empty() )
        return;

    HANDLE file = ::CreateFileW( path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
    if( file == INVALID_HANDLE_VALUE )
        return;

    DWORD written = 0;
    ::WriteFile( file, text.data(), static_cast< DWORD >( text.size() ), &written, nullptr );
    ::CloseHandle( file );
}

[[nodiscard]] std::wstring describeError( const DWORD code )
{
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
        reinterpret_cast< wchar_t* >( &buffer ), 0, nullptr );

    std::wstring text;
    if( length > 0 && buffer != nullptr )
    {
        text.assign( buffer, length );
        while( !text.empty() && ( text.back() == L'\r' || text.back() == L'\n' ) )
            text.pop_back();
    }
    if( buffer != nullptr )
        ::LocalFree( buffer );

    if( text.empty() )
    {
        wchar_t fallback[ 32 ] = {};
        ::swprintf_s( fallback, L"오류 코드 %lu", code );
        text = fallback;
    }
    return text;
}

/// 재시도해 볼 만한 오류인가. 잠금 계열만 물러서서 다시 시도한다.
[[nodiscard]] bool isTransient( const DWORD code )
{
    switch( code )
    {
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_ALREADY_EXISTS:
        case ERROR_DIR_NOT_EMPTY:
            return true;
        default:
            return false;
    }
}

/// swapAll 에서 이미 옮긴 단계를 되돌린다.
///
/// 새 파일은 staging 으로 돌려놓고 구 파일을 제자리에 복구한다. staging 을
/// 온전하게 남겨 두면 다음 실행에서 400MB 를 다시 받지 않고 재시도할 수 있다.
void undoSteps( const std::vector< SwapStep >& steps, Log& log )
{
    for( size_t index = steps.size(); index > 0; --index )
    {
        const SwapStep& step = steps[ index - 1 ];
        log.line( L"되돌리는 중: %s", step.live.c_str() );
        if( pathExists( step.live ) )
            static_cast< void >( moveWithRetry( step.live, step.staged, log ) );
        if( pathExists( step.backup ) )
            static_cast< void >( moveWithRetry( step.backup, step.live, log ) );
    }
}

}  // namespace

// ── Log ───────────────────────────────────────────────────

Log::Log( std::wstring path )
    : path_( std::move( path ) )
{
}

void Log::line( const wchar_t* format, ... )
{
    wchar_t body[ 2048 ] = {};

    va_list args;
    va_start( args, format );
    ::_vsnwprintf_s( body, _TRUNCATE, format, args );
    va_end( args );

    SYSTEMTIME now{};
    ::GetLocalTime( &now );

    wchar_t stamped[ 2200 ] = {};
    ::swprintf_s( stamped, L"%04u-%02u-%02u %02u:%02u:%02u  %s\r\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, body );

    appendUtf8( path_, toUtf8( stamped ) );
}

void Log::setFailure( std::wstring id, std::wstring detail )
{
    failure_       = std::move( id );
    failureDetail_ = std::move( detail );
}

// ── 경로 ──────────────────────────────────────────────────

bool pathExists( const std::wstring& path )
{
    return path.empty() ? false : ::GetFileAttributesW( path.c_str() ) != INVALID_FILE_ATTRIBUTES;
}

bool isDirectory( const std::wstring& path )
{
    const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES
                                          : ::GetFileAttributesW( path.c_str() );
    return attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
}

std::wstring joinPath( const std::wstring& left, const std::wstring& right )
{
    if( left.empty() )
        return right;
    if( right.empty() )
        return left;

    std::wstring result = left;
    if( result.back() != L'\\' && result.back() != L'/' )
        result += L'\\';
    result += right;
    return result;
}

std::vector< std::wstring > listTopLevel( const std::wstring& directory )
{
    std::vector< std::wstring > names;

    WIN32_FIND_DATAW found{};
    HANDLE handle = ::FindFirstFileW( joinPath( directory, L"*" ).c_str(), &found );
    if( handle == INVALID_HANDLE_VALUE )
        return names;

    do
    {
        const std::wstring name = found.cFileName;
        if( name == L"." || name == L".." )
            continue;
        names.push_back( name );
    } while( ::FindNextFileW( handle, &found ) );

    ::FindClose( handle );
    return names;
}

bool removeRecursively( const std::wstring& path )
{
    if( !pathExists( path ) )
        return true;

    if( !isDirectory( path ) )
    {
        // 읽기 전용 특성이 걸려 있으면 DeleteFileW 가 실패한다.
        ::SetFileAttributesW( path.c_str(), FILE_ATTRIBUTE_NORMAL );
        return ::DeleteFileW( path.c_str() ) != 0;
    }

    bool ok = true;
    for( const std::wstring& name : listTopLevel( path ) )
    {
        if( !removeRecursively( joinPath( path, name ) ) )
            ok = false;
    }

    ::SetFileAttributesW( path.c_str(), FILE_ATTRIBUTE_NORMAL );
    if( !::RemoveDirectoryW( path.c_str() ) )
        ok = false;
    return ok;
}

// ── 프로세스 대기 ─────────────────────────────────────────

bool waitForProcessExit( const unsigned long long processId, const unsigned long timeoutMs, Log& log )
{
    if( processId == 0 )
        return true;

    HANDLE process = ::OpenProcess( SYNCHRONIZE, FALSE, static_cast< DWORD >( processId ) );
    if( process == nullptr )
    {
        // 핸들을 열 수 없다 = 이미 사라졌다고 본다. (권한 부족일 수도 있지만,
        // 우리를 띄운 것이 그 프로세스이므로 그 경우는 없다.)
        log.line( L"pid %llu 는 이미 종료된 것으로 보인다.", processId );
        return true;
    }

    const DWORD result = ::WaitForSingleObject( process, timeoutMs );
    ::CloseHandle( process );

    if( result == WAIT_OBJECT_0 )
    {
        log.line( L"pid %llu 가 종료되었다.", processId );
        return true;
    }

    log.line( L"pid %llu 가 %lu ms 안에 종료되지 않았다 (wait=%lu).", processId, timeoutMs, result );
    return false;
}

// ── 이동 ──────────────────────────────────────────────────

bool moveWithRetry( const std::wstring& from, const std::wstring& to, Log& log )
{
    DWORD lastError = ERROR_SUCCESS;

    for( unsigned attempt = 0; attempt < kMaxAttempts; ++attempt )
    {
        if( attempt > 0 )
            ::Sleep( kRetryDelayMs );

        // 대상이 남아 있으면 먼저 치운다. MOVEFILE_REPLACE_EXISTING 은 파일에만
        // 통하고 디렉터리에는 통하지 않기 때문이다.
        if( pathExists( to ) && !removeRecursively( to ) )
        {
            lastError = ::GetLastError();
            continue;
        }

        DWORD flags = 0;
        if( !isDirectory( from ) )
        {
            // 파일은 볼륨이 달라도 옮길 수 있게 해 준다. 디렉터리에 이 플래그를
            // 주면 실패하므로 파일에만 붙인다.
            flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED;
        }

        if( ::MoveFileExW( from.c_str(), to.c_str(), flags ) )
            return true;

        lastError = ::GetLastError();
        if( !isTransient( lastError ) )
            break;
    }

    const std::wstring reason = describeError( lastError );
    log.line( L"이동 실패: %s -> %s (%s)", from.c_str(), to.c_str(), reason.c_str() );
    // 사유는 식별자로, Windows 가 준 설명은 detail 로. 앱이 자기 언어로 문장을
    // 만들고 그 뒤에 설명을 괄호로 붙인다.
    log.setFailure( FailureId::kSwapFailed, reason );
    return false;
}

// ── 교체 ──────────────────────────────────────────────────

bool swapAll( const std::wstring& staging, const std::wstring& target, const std::wstring& backup,
              const std::vector< std::wstring >& removals, Log& log )
{
    if( !isDirectory( staging ) )
    {
        log.line( L"준비 폴더가 없다: %s", staging.c_str() );
        log.setFailure( FailureId::kNoStaging );
        return false;
    }

    // 이전 백업이 남아 있으면 지운다. 두 세대가 섞이면 되돌릴 때 엉뚱한 파일이
    // 복원된다.
    if( pathExists( backup ) && !removeRecursively( backup ) )
    {
        log.line( L"이전 백업을 지울 수 없다: %s", backup.c_str() );
        log.setFailure( FailureId::kBackupDirty );
        return false;
    }
    if( !::CreateDirectoryW( backup.c_str(), nullptr )
        && ::GetLastError() != ERROR_ALREADY_EXISTS )
    {
        const std::wstring reason = describeError( ::GetLastError() );
        log.line( L"백업 폴더를 만들 수 없다: %s (%s)", backup.c_str(), reason.c_str() );
        log.setFailure( FailureId::kBackupCreate, reason );
        return false;
    }

    const std::vector< std::wstring > names = listTopLevel( staging );
    if( names.empty() )
    {
        log.line( L"준비 폴더가 비어 있다: %s", staging.c_str() );
        log.setFailure( FailureId::kStagingEmpty );
        return false;
    }

    log.line( L"최상위 항목 %zu 개를 교체한다.", names.size() );

    std::vector< SwapStep > done;
    done.reserve( names.size() );

    for( const std::wstring& name : names )
    {
        SwapStep step;
        step.live   = joinPath( target, name );
        step.backup = joinPath( backup, name );
        step.staged = joinPath( staging, name );

        if( pathExists( step.live ) && !moveWithRetry( step.live, step.backup, log ) )
        {
            undoSteps( done, log );
            return false;
        }

        if( !moveWithRetry( step.staged, step.live, log ) )
        {
            // 방금 치운 구 파일을 먼저 제자리로 돌려놓는다.
            if( pathExists( step.backup ) )
                static_cast< void >( moveWithRetry( step.backup, step.live, log ) );
            undoSteps( done, log );
            return false;
        }

        done.push_back( step );
    }

    // 이번 버전에서 사라진 파일만 추가로 치운다. 목록에 없는 항목은 절대
    // 건드리지 않는다 — 사용자 설정(.ini)과 Environment/ 가 그 규칙으로 남는다.
    for( const std::wstring& name : removals )
    {
        const std::wstring live = joinPath( target, name );
        if( !pathExists( live ) )
            continue;

        log.line( L"삭제 목록 항목을 치운다: %s", name.c_str() );
        // 실패해도 치명적이지 않다. 구버전 파일 하나가 남을 뿐이다.
        static_cast< void >( moveWithRetry( live, joinPath( backup, name ), log ) );
    }

    log.line( L"교체를 마쳤다." );
    return true;
}

bool rollbackAll( const std::wstring& backup, const std::wstring& target, Log& log )
{
    if( !isDirectory( backup ) )
    {
        log.line( L"되돌릴 백업이 없다: %s", backup.c_str() );
        log.setFailure( FailureId::kNoBackup );
        return false;
    }

    const std::vector< std::wstring > names = listTopLevel( backup );
    if( names.empty() )
    {
        log.line( L"백업 폴더가 비어 있다: %s", backup.c_str() );
        log.setFailure( FailureId::kBackupEmpty );
        return false;
    }

    bool ok = true;
    for( const std::wstring& name : names )
    {
        // 사용자가 명시적으로 되돌리기를 요청한 상황이다. 새 버전 파일은 못 쓰는
        // 상태로 보고 그대로 덮어쓴다.
        if( !moveWithRetry( joinPath( backup, name ), joinPath( target, name ), log ) )
            ok = false;
    }

    log.line( ok ? L"되돌리기를 마쳤다." : L"일부 항목을 되돌리지 못했다." );
    return ok;
}

// ── 결과 / 재실행 ─────────────────────────────────────────

void writeResult( const std::wstring& path, const bool succeeded, const std::wstring& version,
                  const std::wstring& error, const std::wstring& detail )
{
    if( path.empty() )
        return;

    // 값에 개행이 들어가면 INI 한 줄이 깨진다.
    const auto flatten = []( const std::wstring& text ) {
        std::wstring flat = text;
        for( wchar_t& ch : flat )
        {
            if( ch == L'\r' || ch == L'\n' )
                ch = L' ';
        }
        return flat;
    };

    wchar_t body[ 2600 ] = {};
    // QSettings(IniFormat) 는 그룹 없는 키를 [General] 에서 읽는다.
    // error 는 FailureId 값이고 앱이 번역한다. errorDetail 은 Windows 가 준
    // 설명처럼 번역할 수 없는 부가 정보다.
    ::swprintf_s( body, L"[General]\r\nsucceeded=%s\r\nversion=%s\r\nerror=%s\r\nerrorDetail=%s\r\n",
                 succeeded ? L"true" : L"false", version.c_str(),
                 flatten( error ).c_str(), flatten( detail ).c_str() );

    // 결과 파일은 매번 새로 쓴다.
    HANDLE file = ::CreateFileW( path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
    if( file == INVALID_HANDLE_VALUE )
        return;

    const std::string utf8 = toUtf8( body );
    DWORD written = 0;
    ::WriteFile( file, utf8.data(), static_cast< DWORD >( utf8.size() ), &written, nullptr );
    ::CloseHandle( file );
}

bool relaunch( const std::wstring& executablePath, Log& log )
{
    if( executablePath.empty() || !pathExists( executablePath ) )
    {
        log.line( L"재실행할 파일이 없다: %s", executablePath.c_str() );
        return false;
    }

    // 작업 디렉터리를 exe 위치로 준다. 앱이 applicationDirPath() 기준으로
    // Environment/ 와 ini 를 찾으므로 실제로는 무관하지만, 상대 경로를 쓰는
    // 자식 프로세스가 생겼을 때 헷갈리지 않게 맞춰 둔다.
    std::wstring directory = executablePath;
    const size_t slash = directory.find_last_of( L"\\/" );
    if( slash != std::wstring::npos )
        directory.erase( slash );

    std::wstring commandLine = L"\"" + executablePath + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof( startup );
    PROCESS_INFORMATION process{};

    if( !::CreateProcessW( executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                          0, nullptr, directory.empty() ? nullptr : directory.c_str(),
                          &startup, &process ) )
    {
        const std::wstring reason = describeError( ::GetLastError() );
        log.line( L"재실행 실패: %s (%s)", executablePath.c_str(), reason.c_str() );
        return false;
    }

    ::CloseHandle( process.hThread );
    ::CloseHandle( process.hProcess );
    log.line( L"앱을 다시 실행했다: %s", executablePath.c_str() );
    return true;
}

}  // namespace mrst::updater
