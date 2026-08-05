#pragma once

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace mrst {

class UvTask;

Q_NAMESPACE

enum class EnvState
{
    Unknown,        ///< 아직 확인 전
    Checking,
    NotConfigured,  ///< 구성 필요 (기능은 비활성, 앱은 정상 동작)
    Preparing,      ///< 리소스 추출 중
    Syncing,        ///< uv sync 진행 중
    Verifying,      ///< 설치 결과 확인 중
    Ready,
    Failed,
    Cancelled,
};
Q_ENUM_NS( EnvState )

/// EXE 옆 `Environment/` 에 번들 Python 런타임(Sphinx + Esbonio)을 구성한다.
///
/// 정책: **startup 을 절대 막지 않는다.** 콜드 부트스트랩은 관리형 CPython 과
/// Sphinx/테마를 수백 MB 내려받으므로, 앱은 degraded 상태로 즉시 뜨고 준비가
/// 끝나면 프리뷰/Esbonio 기능이 켜진다. 모든 장기 작업은 비동기다.
class PythonEnvManager final : public QObject
{
    Q_OBJECT

public:
    /// ready.marker 스키마. 의존성이 바뀌면 올려서 재동기화를 유도한다.
    static constexpr int                RuntimeSchema = 2;

    explicit PythonEnvManager( QObject* parent = nullptr );
    ~PythonEnvManager() override;

    // ── 경로 ────────────────────────────────────────────────
    [[nodiscard]] QString               runtimeRoot() const;      ///< <exe>/Environment
    [[nodiscard]] QString               projectDir() const;
    [[nodiscard]] QString               venvDir() const;
    [[nodiscard]] QString               pythonExe() const;
    [[nodiscard]] QString               sphinxBuildExe() const;
    [[nodiscard]] QString               esbonioExe() const;
    [[nodiscard]] QString               scriptsDir() const;       ///< <runtimeRoot>/scripts
    [[nodiscard]] QString               previewBuilderScript() const;
    [[nodiscard]] QString               shadowDir() const;        ///< 미저장 버퍼 임시 사본
    [[nodiscard]] QString               cacheDir() const;

    /// qrc 의 헬퍼 스크립트를 scripts/ 로 추출한다. 내용이 같으면 건너뛴다.
    /// 환경이 이미 준비된 상태에서 앱만 업데이트된 경우에도 최신화되도록
    /// 부트스트랩과 별개로 호출한다. (작은 텍스트 파일이라 동기로 충분)
    void                                ensureHelperScripts();
    [[nodiscard]] QString               embeddedUvTarget() const;
    [[nodiscard]] QString               readyMarker() const;
    [[nodiscard]] QString               uvExecutable() const;

    // ── 상태 ────────────────────────────────────────────────
    [[nodiscard]] EnvState              state() const;
    [[nodiscard]] bool                  isReady() const;
    [[nodiscard]] bool                  isBusy() const;
    [[nodiscard]] QString               stateText() const;        ///< 한국어 표시 문자열
    [[nodiscard]] QString               lastError() const;
    [[nodiscard]] QDateTime             configuredDate() const;
    [[nodiscard]] QString               configuredDateText() const;
    [[nodiscard]] QString               uvDescription() const;    ///< 부작용 없음 (파일 안 씀)

    // ── 설정 ────────────────────────────────────────────────
    [[nodiscard]] bool                  useExternalUv() const;
    [[nodiscard]] QString               externalUvPath() const;
    [[nodiscard]] bool                  autoBootstrap() const;
    [[nodiscard]] bool                  installOptionalThemes() const;
    [[nodiscard]] bool                  installOptionalExtensions() const;
    [[nodiscard]] QStringList           extraPackages() const;

    void                                setUseExternalUv( bool enabled );
    void                                setExternalUvPath( const QString& path );
    void                                setAutoBootstrap( bool enabled );
    void                                setInstallOptionalThemes( bool enabled );
    void                                setInstallOptionalExtensions( bool enabled );
    void                                saveUvSettings() const;

    // ── 동작 (전부 비동기) ──────────────────────────────────
    void                                refreshState();
    /// 준비돼 있지 않으면 구성을 시작한다. 이미 진행 중이면 아무 것도 안 한다.
    void                                ensureEnvironmentAsync();
    void                                configureEnvironmentAsync( bool forceRebuild = false );
    void                                cancel();
    void                                requestUvVersionAsync();

signals:
    void                                stateChanged( mrst::EnvState state );
    void                                readyChanged( bool ready );
    /// percent < 0 이면 불확정(진행 중이지만 비율을 알 수 없음).
    void                                progressChanged( int percent, const QString& phase );
    void                                bootstrapLog( const QString& text );
    void                                failed( const QString& message );
    void                                uvVersionReady( const QString& description );

private:
    void                                setState( EnvState next );
    void                                setLastError( const QString& message );
    /// 리소스 추출을 워커 스레드로 넘긴다. 내장 uv.exe 가 67MB 라
    /// GUI 스레드에서 복사하면 첫 페인트가 10초 넘게 밀린다.
    void                                startExtractTask();
    void                                onResourcesExtracted( bool ok, const QString& errorMessage );
    void                                startSyncTask();
    void                                startVerifyTask();
    void                                writeReadyMarker() const;
    void                                updateProgressFromUvLine( const QString& line );
    [[nodiscard]] QStringList           syncArguments() const;
    [[nodiscard]] QProcessEnvironment   uvEnvironment() const;
    [[nodiscard]] QString               embeddedPyprojectHash() const;
    [[nodiscard]] bool                  markerMatchesEmbeddedManifest() const;
    [[nodiscard]] bool                  installedFilesPresent() const;

    EnvState                            state_ = EnvState::Unknown;
    QString                             lastError_;
    bool                                useExternalUv_ = false;
    QString                             externalUvPath_;
    bool                                autoBootstrap_ = true;
    bool                                installOptionalThemes_ = true;
    bool                                installOptionalExtensions_ = true;
    QStringList                         extraPackages_;
    QPointer< UvTask >                  activeTask_;
    bool                                lastReadyState_ = false;
};

}  // namespace mrst
