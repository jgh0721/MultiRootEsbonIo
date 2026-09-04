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
    [[nodiscard]] bool                  isProjectRepairing() const;
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
    /// 앱 종료 전용. uv/python 을 곧바로 죽이고 기다리지 않는다.
    /// 남으면 Environment/ 를 물고 있어 업데이터의 파일 교체가 실패한다
    /// (그 폴더는 앱 설치 폴더 아래다).
    void                                cancelImmediately();
    void                                requestUvVersionAsync();
    /// 지정한 환경에 패키지를 설치한다. targetPythonExe 가 비면 번들 환경.
    /// 번들에 설치한 것은 extraPackages 에 남겨 다음 sync 후 다시 적용한다
    /// (uv sync 는 lock 에 없는 것을 prune 하므로 그러지 않으면 사라진다).
    void                                installPackagesAsync( const QStringList& distributions,
                                                              const QString& targetPythonExe = {} );
    /// 손상된 프로젝트 venv 옆에 교체 환경을 만든 뒤 검증을 통과한 경우에만 바꾼다.
    /// projectRoot 는 venv 의 부모이며 pyproject.toml 이 있어야 한다.
    [[nodiscard]] bool                  repairProjectEnvironmentAsync( const QString& projectKey,
                                                                       const QString& projectRoot,
                                                                       const QString& venvDir );

signals:
    void                                stateChanged( mrst::EnvState state );
    void                                readyChanged( bool ready );
    /// percent < 0 이면 불확정(진행 중이지만 비율을 알 수 없음).
    void                                progressChanged( int percent, const QString& phase );
    void                                bootstrapLog( const QString& text );
    void                                failed( const QString& message );
    void                                uvVersionReady( const QString& description );
    void                                packageInstallFinished( bool success, const QStringList& distributions );
    void                                projectRepairStarted( const QString& projectKey );
    void                                projectRepairProgress( const QString& projectKey, int percent,
                                                               const QString& phase );
    void                                projectRepairFinished( const QString& projectKey, bool success,
                                                               const QString& message );

private:
    void                                setState( EnvState next );
    void                                setLastError( const QString& message );
    /// 리소스 추출을 워커 스레드로 넘긴다. 내장 uv.exe 가 67MB 라
    /// GUI 스레드에서 복사하면 첫 페인트가 10초 넘게 밀린다.
    void                                startExtractTask();
    void                                onResourcesExtracted( bool ok, const QString& errorMessage );
    void                                startSyncTask();
    void                                startVerifyTask();
    void                                startProjectRepairSync();
    void                                startProjectRepairVerify();
    void                                finishProjectRepair( bool success, const QString& message );
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
    /// 이미 Ready 인 환경의 첫 실행 검증이 실패하면 한 번만 자동 재구성한다.
    bool                                repairBundledAfterVerifyFailure_ = false;
    QPointer< UvTask >                  projectRepairTask_;
    QString                             projectRepairKey_;
    QString                             projectRepairRoot_;
    QString                             projectRepairOriginalDir_;
    QString                             projectRepairCandidateDir_;
    QString                             projectRepairBackupDir_;
    qint64                              projectRepairOriginalCfgSize_ = -1;
    qint64                              projectRepairOriginalCfgMTimeMs_ = -1;
    bool                                lastReadyState_ = false;
};

}  // namespace mrst
