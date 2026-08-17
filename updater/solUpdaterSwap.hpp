#pragma once

#include <string>
#include <vector>

/// 업데이터 전용 파일 조작.
///
/// **Qt 를 쓰지 않는다.** 업데이터가 Qt6Core.dll 을 로드하면 자기가 교체해야
/// 하는 파일을 스스로 잠근다. kernel32/shell32 와 정적 CRT 만으로 만든다.
namespace mrst::updater {

/// 실패 사유 식별자.
///
/// 사람이 읽을 문장이 아니라 **앱이 번역할 키**다. 여기에 한국어 문장을 쓰면
/// 그 문장이 그대로 result.ini 에 들어가고 앱은 그것을 대화상자에 붙인다.
/// 그러면 사용자가 앱에서 일본어를 골라도 그 한 줄만 한국어로 남는다.
///
/// 앱의 UpdateInstaller::describeFailure() 가 이 값을 문장으로 바꾼다. 모르는
/// 값은 그대로 통과시키므로, 구버전 업데이터가 남긴 완성 문장도 계속 보인다
/// (교체 직전에 도는 업데이터는 **항상 구버전**이다).
namespace FailureId
{
    inline constexpr auto kAppStillAlive = L"updater.appStillAlive";
    inline constexpr auto kNoStaging     = L"updater.noStaging";
    inline constexpr auto kStagingEmpty  = L"updater.stagingEmpty";
    inline constexpr auto kBackupDirty   = L"updater.backupDirty";
    inline constexpr auto kBackupCreate  = L"updater.backupCreateFailed";
    inline constexpr auto kNoBackup      = L"updater.noBackup";
    inline constexpr auto kBackupEmpty   = L"updater.backupEmpty";
    inline constexpr auto kSwapFailed    = L"updater.swapFailed";
}   // namespace FailureId

/// 로그 파일에 한 줄씩 덧붙인다 (UTF-8).
///
/// 업데이터는 창이 없다. 무엇이 왜 실패했는지는 이 파일과 result.ini 로만
/// 남으므로, 판단이 갈리는 지점마다 기록해 둔다.
class Log
{
public:
    explicit Log( std::wstring path );

    void                        line( const wchar_t* format, ... );
    /// 마지막으로 기록한 실패. id 는 FailureId 중 하나이고 result.ini 의 error
    /// 값이 된다. detail 은 Windows 가 준 설명처럼 번역할 수 없는 부가 정보로,
    /// errorDetail 로 따로 나간다 (비워도 된다).
    void                        setFailure( std::wstring id, std::wstring detail = {} );
    [[nodiscard]] const std::wstring& failure() const { return failure_; }
    [[nodiscard]] const std::wstring& failureDetail() const { return failureDetail_; }

private:
    std::wstring                path_;
    std::wstring                failure_;
    std::wstring                failureDetail_;
};

[[nodiscard]] bool              pathExists( const std::wstring& path );
[[nodiscard]] bool              isDirectory( const std::wstring& path );
[[nodiscard]] std::wstring      joinPath( const std::wstring& left, const std::wstring& right );
/// 디렉터리의 최상위 항목 이름 (`.` `..` 제외).
[[nodiscard]] std::vector< std::wstring > listTopLevel( const std::wstring& directory );
/// 파일이든 디렉터리든 지운다. 없으면 성공으로 본다.
[[nodiscard]] bool              removeRecursively( const std::wstring& path );

/// 그 프로세스가 끝날 때까지 기다린다.
///
/// 타임아웃을 넉넉히 주어야 한다. 앱의 종료 경로는 esbonio 프로세스마다
/// terminate(1.5초) + kill(1.5초) 를 기다리므로 기본 설정에서도 최악 9초가
/// 걸린다.
[[nodiscard]] bool              waitForProcessExit( unsigned long long processId,
                                                    unsigned long timeoutMs, Log& log );

/// 이름을 바꾼다. 실패하면 물러서며 다시 시도한다.
///
/// 백신이나 탐색기가 방금 나타난 파일을 잠깐 물고 있는 경우가 흔해서
/// (ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED) 한 번의 실패로 포기하면
/// 멀쩡한 업데이트가 자주 깨진다.
[[nodiscard]] bool              moveWithRetry( const std::wstring& from, const std::wstring& to,
                                               Log& log );

/// 되돌리기 위해 기억해 두는 한 단계.
struct SwapStep
{
    std::wstring                live;      ///< 설치 폴더의 제자리
    std::wstring                backup;    ///< 밀어 둔 구 버전
    std::wstring                staged;    ///< 원래 있던 준비 폴더 위치 (되돌릴 때 여기로 돌려놓는다)
};

/// staging 의 최상위 항목을 target 으로 밀어 넣는다.
///
/// 실패하면 그때까지 옮긴 것을 전부 되돌린다. removals 에 적힌 이름만 추가로
/// 치우고, **모르는 최상위 항목은 절대 건드리지 않는다** — 사용자 설정 파일과
/// 수백 MB 짜리 파이썬 환경(Environment/)이 그 규칙으로 보호된다.
[[nodiscard]] bool              swapAll( const std::wstring& staging, const std::wstring& target,
                                         const std::wstring& backup,
                                         const std::vector< std::wstring >& removals, Log& log );

/// backup 의 항목을 target 으로 되돌린다 (--rollback).
[[nodiscard]] bool              rollbackAll( const std::wstring& backup, const std::wstring& target,
                                             Log& log );

/// 앱이 읽을 결과 파일을 쓴다. QSettings(IniFormat) 이 읽으므로 [General] 섹션에 넣는다.
///
/// error 는 FailureId 값이고, detail 은 번역할 수 없는 부가 정보다
/// (Windows 오류 설명 등). 앱이 error 를 자기 언어 문장으로 바꾸고 detail 을
/// 괄호에 덧붙인다.
void                            writeResult( const std::wstring& path, bool succeeded,
                                             const std::wstring& version, const std::wstring& error,
                                             const std::wstring& detail = {} );

/// 앱을 다시 띄운다.
[[nodiscard]] bool              relaunch( const std::wstring& executablePath, Log& log );

}  // namespace mrst::updater
