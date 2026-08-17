#pragma once

#include <QCryptographicHash>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include "core/solUpdateManifest.hpp"

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

namespace mrst {

class UpdateInstaller;
class UvTask;

/// GitHub 릴리스에 올려 둔 update-manifest.json 을 보고 새 버전을 내려받아
/// staging 에 풀어 두는 서비스.
///
/// **startup 을 절대 막지 않는다.** PythonEnvManager 와 같은 원칙이다. 점검은
/// 창이 뜬 뒤에 지연 실행되고, 오프라인·타임아웃·릴리스 없음(404)·매니페스트
/// 파손은 로그에만 남기고 조용히 Idle 로 되돌아간다. 사용자가 직접 누른
/// 경우(userInitiated)만 결과를 알린다.
///
/// 파일 교체는 이 클래스가 하지 않는다 — UpdateInstaller 가 별도 실행 파일
/// (mrst_updater.exe)에 맡긴다.
class UpdateService final : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Idle,               ///< 점검 전 / 점검했지만 할 일이 없음
        Checking,           ///< 매니페스트 조회 중
        UpdateAvailable,    ///< 새 버전 확인. 사용자 승인 대기
        Downloading,        ///< ZIP 수신 중 (진행률이 의미 있는 유일한 구간)
        Extracting,         ///< bsdtar -x
        Verifying,          ///< 서명 / 파일 버전 확인
        ReadyToInstall,     ///< staging 검증 완료. 앱을 닫으면 교체된다
        Installing,         ///< 업데이터를 띄웠다. 곧 이 프로세스는 죽는다
        Failed,
    };
    Q_ENUM( State )

    explicit UpdateService( QObject* parent = nullptr );
    ~UpdateService() override;

    [[nodiscard]] State                 state() const { return state_; }
    /// 진행 중이라 새 요청을 받을 수 없는 상태인가.
    [[nodiscard]] bool                  isBusy() const;
    [[nodiscard]] UpdateInfo            available() const { return info_; }
    [[nodiscard]] QString               lastError() const { return lastError_; }

    // ── 설정 (INI [update]) ─────────────────────────────────
    /// 0 이면 확인하지 않는다.
    [[nodiscard]] int                   checkIntervalDays() const;
    [[nodiscard]] QDateTime             lastCheckedAt() const;
    [[nodiscard]] QString               skippedVersion() const;
    [[nodiscard]] bool                  isDueForCheck() const;
    [[nodiscard]] QUrl                  manifestUrl() const;
    /// 설정 대화상자가 값을 바꾼 뒤 호출한다.
    void                                reloadSettings();
    /// 지금 알린 버전을 다시 알리지 않는다.
    void                                skipAvailableVersion();

    // ── 동작 (전부 비동기) ──────────────────────────────────
    /// userInitiated == true 면 주기와 skippedVersion 을 무시하고, 실패도 알린다.
    void                                checkAsync( bool userInitiated );
    void                                downloadAsync();
    void                                cancel();
    /// 기동 직후 1회. 지난 설치 결과를 보고하고, 준비된 staging 이 있으면
    /// ReadyToInstall 로 복원하며, 설치가 끝난 뒤 남은 backup 을 정리한다.
    void                                reconcileAfterRestart();
    /// 업데이터를 띄운다. true 를 받으면 호출자는 **즉시** 앱을 닫아야 한다.
    [[nodiscard]] bool                  launchInstaller();

    /// 상태를 사람이 읽는 한 마디로. 상태바 표시에 쓴다.
    [[nodiscard]] QString               phaseText() const;

signals:
    void                                stateChanged( mrst::UpdateService::State state );
    void                                updateFound( const mrst::UpdateInfo& info );
    void                                upToDate( bool userInitiated );
    /// percent < 0 이면 불확정. PythonEnvManager::progressChanged 와 같은 규약.
    void                                progressChanged( int percent, const QString& phase );
    void                                readyToInstall( const mrst::UpdateInfo& info );
    /// silent == true 면 UI 를 띄우지 않고 로그만 남긴다 (기동 시 오프라인 등).
    void                                failed( const QString& message, bool silent );
    void                                logMessage( const QString& text );
    /// 지난 실행의 설치 결과. reconcileAfterRestart() 에서 한 번만 나온다.
    void                                installOutcomeReported( bool succeeded, const QString& version,
                                                                const QString& message );

private:
    void                                setState( State next );
    void                                fail( const QString& message );
    void                                log( const QString& message );
    [[nodiscard]] QNetworkAccessManager* networkManager();
    void                                abortReply();
    void                                writeLastCheckedNow();

    void                                onManifestFinished();
    void                                onDownloadFinished();
    void                                startExtract();
    void                                startVerification();
    void                                onVerified( bool ok, const QString& message );
    /// staging 에 이미 준비된 버전이 있으면 ReadyToInstall 로 복원한다.
    /// 복원했으면 true. 기동 시 정리 경로에서는 결과를 쓰지 않는다.
    bool                                restoreStagedIfUsable( const QString& currentVersion );

    State                               state_ = State::Idle;
    UpdateInfo                          info_;
    QString                             lastError_;
    bool                                userInitiated_ = false;

    UpdateInstaller*                    installer_ = nullptr;
    QNetworkAccessManager*              network_ = nullptr;   ///< 지연 생성
    QPointer< QNetworkReply >           reply_;
    std::unique_ptr< QSaveFile >        zipFile_;
    QCryptographicHash                  hash_{ QCryptographicHash::Sha256 };
    QString                             zipPath_;
    QPointer< UvTask >                  extractTask_;
    int                                 extractedEntries_ = 0;
};

}  // namespace mrst
