#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace mrst {

/// 지난 실행에서 업데이터가 남긴 결과. 새 버전이 처음 뜰 때 읽어 보고한다.
struct InstallOutcome
{
    bool                                attempted = false;   ///< 결과 파일이 있었다 = 설치를 시도했다
    bool                                succeeded = false;
    QString                             version;
    QString                             errorMessage;
};

/// staging 에 풀어 둔 배포물의 정보.
struct StagedUpdate
{
    QString                             version;
    QStringList                         removals;            ///< 이번 버전에서 사라진 최상위 항목
    /// staging 에 실제 파일이 남아 있는가.
    ///
    /// 교체가 끝나면 staging 은 비고 이 정보 파일만 남는다. 그 상태를 "준비물
    /// 없음" 으로 뭉뚱그리면 설치 뒤에 백업을 정리할 기회를 놓쳐 400MB 가
    /// 영원히 남는다. 그래서 "무엇을 준비했는가" 와 "그게 아직 있는가" 를
    /// 따로 알린다.
    bool                                payloadPresent = false;

    [[nodiscard]] bool                  isValid() const { return !version.isEmpty(); }
};

/// 업데이트 작업 디렉터리와 설치(파일 교체) 절차를 담당한다.
///
/// 작업 공간을 앱 디렉터리 안(`<appdir>/.update`)에 두는 이유: 교체가 같은
/// 볼륨의 rename 이 되어 400MB 를 다시 복사하지 않고, 백신이 같은 파일을 두 번
/// 전수 검사하지도 않는다. %LOCALAPPDATA% 를 무조건 쓰면 이 앱처럼 포터블
/// 배포라 D:/E: 에 풀어 쓰는 경우 볼륨을 넘어가는 복사가 된다.
///
/// 파일 교체 자체는 이 클래스가 하지 않는다. 실행 중인 프로세스는 자기 exe 와
/// 로드된 DLL 을 지울 수 없고, 부분 실패를 되돌릴 주체도 필요하기 때문에
/// 별도 실행 파일(mrst_updater.exe)에 맡긴다.
class UpdateInstaller final : public QObject
{
    Q_OBJECT

public:
    explicit UpdateInstaller( QObject* parent = nullptr );
    ~UpdateInstaller() override;

    [[nodiscard]] static QString        appDirectory();
    [[nodiscard]] static QString        applicationExecutable();
    /// 앱과 같은 폴더에 배포되는 업데이터. 실행할 때는 %TEMP% 사본을 쓴다
    /// (자기 자신도 교체 대상이라 원본을 잠그면 안 된다).
    [[nodiscard]] static QString        updaterExecutable();

    /// 앱 디렉터리에 실제로 파일을 만들어 보고 판정한다.
    ///
    /// ACL 을 해석해서 추측하지 않는다 — 추측은 틀린다. 64비트 프로세스는
    /// Program Files 에 대해 파일 가상화를 받지 않으므로 이 시도가 곧바로
    /// 실패한다.
    [[nodiscard]] bool                  isAppDirectoryWritable() const;

    [[nodiscard]] QString               workRoot() const;
    [[nodiscard]] QString               downloadDirectory() const;
    [[nodiscard]] QString               stagingDirectory() const;
    [[nodiscard]] QString               backupDirectory() const;
    [[nodiscard]] QString               updaterLogPath() const;
    [[nodiscard]] QString               resultPath() const;
    [[nodiscard]] QString               downloadPath( const QString& assetName ) const;

    /// 작업 디렉터리를 만든다.
    bool                                prepareWorkspace( QString* errorMessage );
    /// staging 을 비우고 새로 만든다. 이전 시도의 잔여물이 섞이면 검증이 통과한
    /// 채로 엉뚱한 파일이 설치된다.
    bool                                resetStaging( QString* errorMessage );

    /// 다른 인스턴스가 업데이트 중이면 false. 아니면 우리 pid 로 락을 잡는다.
    [[nodiscard]] bool                  acquireLock();
    void                                releaseLock();

    [[nodiscard]] StagedUpdate          stagedUpdate() const;
    bool                                writeStagedUpdate( const StagedUpdate& staged, QString* errorMessage );
    void                                clearStagedUpdate();

    /// 업데이터를 띄운다. true 를 받으면 호출자는 **즉시** 앱을 닫아야 한다.
    [[nodiscard]] bool                  launchUpdater( bool relaunchApp, QString* errorMessage );

    /// 결과 파일을 읽고 지운다. 두 번 보고하지 않기 위해 읽으면서 치운다.
    [[nodiscard]] InstallOutcome        takePreviousOutcome();
    /// 업데이터가 result.ini 에 남긴 실패 식별자를 사용자의 언어 문장으로 바꾼다.
    /// 모르는 값은 그대로 돌려준다 — 구버전 업데이터는 완성된 문장을 남긴다.
    [[nodiscard]] static QString        describeFailure( const QString& id, const QString& detail );

    /// 큰 디렉터리나 파일을 작업 스레드에서 지운다.
    /// (400MB 를 GUI 스레드에서 지우면 첫 페인트가 눈에 보이게 밀린다 —
    ///  PythonEnvManager 가 67MB 복사에서 겪은 것과 같은 문제다.)
    void                                removePathsAsync( const QStringList& paths );

signals:
    void                                logMessage( const QString& text );

private:
    [[nodiscard]] QString               stagedInfoPath() const;
    [[nodiscard]] QString               lockPath() const;

    bool                                lockHeld_ = false;
};

}  // namespace mrst
