#include "stdafx.h"
#include "core/solUpdateService.hpp"

#include "core/solAppSettings.hpp"
#include "core/solUpdateInstaller.hpp"
#include "core/solUvTaskRunner.hpp"
#include "utils/AuthenticodeVerifier.hpp"
#include "utils/solBackgroundWork.hpp"

#include "mrst_version.h"

// QtNetwork 는 stdafx.h(PCH)에 넣지 않는다. PCH 를 건드리면 전체 리빌드가
// 걸리는데, 네트워크 타입이 필요한 번역 단위는 이 파일 하나다.
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThreadPool>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace mrst {

namespace {

constexpr auto kIntervalKey    = "update/checkIntervalDays";
constexpr auto kLastCheckedKey = "update/lastCheckedAt";
constexpr auto kSkippedKey     = "update/skippedVersion";
constexpr auto kManifestUrlKey = "update/manifestUrl";

/// 응답이 이 시간 동안 한 글자도 오지 않으면 끊는다.
///
/// 전체 시간 제한이 아니다. 배포 파일이 150MB 라 느린 회선에서는 수십 분이
/// 정상이고, 총량으로 끊으면 그런 사용자는 영원히 업데이트할 수 없다.
constexpr int  kTransferTimeoutMs = 30 * 1000;

/// ZIP 이 풀렸을 때의 크기 추정 배수. 매니페스트가 해제 후 크기를 담지 않으므로
/// 디스크 여유를 볼 때 쓴다. 실측(399MB 페이로드 / 약 150MB ZIP)보다 넉넉하게 잡는다.
constexpr qint64 kEstimatedExpansionFactor = 3;

/// 매니페스트 본문의 상한. 이 파일은 1KB 남짓이라 이보다 크면 뭔가 잘못된 것이다
/// (릴리스가 없을 때 GitHub 이 주는 HTML 페이지 등).
constexpr qint64 kMaxManifestBytes = 256 * 1024;

/// zip 을 풀 수 있는 bsdtar 의 절대 경로.
///
/// PATH 로 tar 를 찾으면 안 된다. Git for Windows 의 GNU tar 가 앞서 있으면
/// zip 을 "does not look like a tar archive" 로 거부한다. 실제로 이 개발 장비의
/// PATH 가 그 상태다. Windows 10 1803 이후의 System32\tar.exe 는 libarchive
/// 기반 bsdtar 이고 zip 을 읽는다 (Qt 6.11 자체가 Win10 1809+ 를 요구하므로
/// 이 파일이 없는 환경은 사실상 없다).
[[nodiscard]] QString systemBsdTarPath()
{
#ifdef Q_OS_WIN
    wchar_t buffer[ MAX_PATH ] = {};
    const UINT length = ::GetSystemDirectoryW( buffer, MAX_PATH );
    if( length == 0 || length >= MAX_PATH )
        return {};

    const QString path = QDir( QString::fromWCharArray( buffer, static_cast< int >( length ) ) )
                            .filePath( QStringLiteral( "tar.exe" ) );
    return QFileInfo::exists( path ) ? QDir::toNativeSeparators( path ) : QString();
#else
    return {};
#endif
}

[[nodiscard]] QString userAgent()
{
    // 공백 없는 제품 식별자를 쓴다. MRST_INTERNAL_NAME 은 배포 파일 이름과 같아서
    // 공백이 들어 있고, User-Agent 의 product 토큰에는 공백을 넣을 수 없다(RFC 9110).
    return QStringLiteral( "%1/%2" )
        .arg( QStringLiteral( MRST_UPDATE_PRODUCT_ID ), QStringLiteral( MRST_VERSION_STRING ) );
}

}  // namespace

UpdateService::UpdateService( QObject* parent )
    : QObject( parent )
    , installer_( new UpdateInstaller( this ) )
{
    connect( installer_, &UpdateInstaller::logMessage, this, &UpdateService::logMessage );
}

UpdateService::~UpdateService()
{
    // 소멸 중에는 시그널을 내보내지 않는다. 진행 중인 것만 끊는다.
    if( reply_ != nullptr )
    {
        reply_->disconnect( this );
        reply_->abort();
    }
    if( extractTask_ != nullptr )
        extractTask_->cancel();
    if( zipFile_ != nullptr )
        zipFile_->cancelWriting();
}

// ── 상태 ──────────────────────────────────────────────────

bool UpdateService::isBusy() const
{
    switch( state_ )
    {
        case State::Checking:
        case State::Downloading:
        case State::Extracting:
        case State::Verifying:
        case State::Installing:
            return true;
        default:
            return false;
    }
}

void UpdateService::setState( const State next )
{
    if( state_ == next )
        return;
    state_ = next;
    emit stateChanged( state_ );
}

QString UpdateService::phaseText() const
{
    switch( state_ )
    {
        case State::Checking:        return tr( "확인 중" );
        case State::UpdateAvailable: return tr( "새 버전 있음" );
        case State::Downloading:     return tr( "내려받는 중" );
        case State::Extracting:      return tr( "압축 푸는 중" );
        case State::Verifying:       return tr( "검사 중" );
        case State::ReadyToInstall:  return tr( "설치 준비 완료" );
        case State::Installing:      return tr( "설치 중" );
        case State::Failed:          return tr( "실패" );
        case State::Idle:            break;
    }
    return {};
}

void UpdateService::log( const QString& message )
{
    emit logMessage( tr( "[업데이트] %1" ).arg( message ) );
}

void UpdateService::fail( const QString& message )
{
    lastError_ = message;
    // 사용자가 직접 누르지 않았다면 조용히 넘어간다. 편집기를 쓰는 도중에
    // 네트워크 사정으로 대화상자가 뜨는 것은 방해일 뿐이다.
    const bool silent = !userInitiated_;
    log( message );
    setState( State::Failed );
    emit failed( message, silent );
    // Failed 는 최종 상태가 아니다. 다음 점검을 받을 수 있게 되돌린다.
    setState( State::Idle );
}

// ── 설정 ──────────────────────────────────────────────────

int UpdateService::checkIntervalDays() const
{
    // 설정 파일은 사람이 고칠 수 있다. 음수나 10000 이 들어와도 깨지지 않게 한다.
    return clampCheckIntervalDays(
        AppSettings().value( QLatin1String( kIntervalKey ), kDefaultCheckIntervalDays ).toInt() );
}

QDateTime UpdateService::lastCheckedAt() const
{
    return QDateTime::fromString(
        AppSettings().value( QLatin1String( kLastCheckedKey ) ).toString(), Qt::ISODate );
}

QString UpdateService::skippedVersion() const
{
    return AppSettings().value( QLatin1String( kSkippedKey ) ).toString().trimmed();
}

QUrl UpdateService::manifestUrl() const
{
    const QString custom = AppSettings().value( QLatin1String( kManifestUrlKey ) ).toString().trimmed();
    if( custom.isEmpty() )
        return defaultManifestUrl();

    const QUrl url( custom );
    if( !url.isValid() || url.host().isEmpty() )
    {
        // 사람이 잘못 적었다. 조용히 기본값으로 돌아가되 흔적은 남긴다.
        return defaultManifestUrl();
    }
    return url;
}

bool UpdateService::isDueForCheck() const
{
    return isCheckDue( lastCheckedAt(), checkIntervalDays(), QDateTime::currentDateTimeUtc() );
}

void UpdateService::reloadSettings()
{
    // 지금은 읽을 때마다 INI 를 보므로 따로 캐시할 것이 없다. 주기를 껐다면
    // 진행 중인 작업까지 멈추지는 않는다 (사용자가 이미 승인한 작업이다).
}

void UpdateService::skipAvailableVersion()
{
    if( info_.version.isEmpty() )
        return;

    AppSettings settings;
    settings.setValue( QLatin1String( kSkippedKey ), info_.version );
    log( tr( "%1 버전을 건너뜁니다." ).arg( info_.version ) );
    setState( State::Idle );
}

void UpdateService::writeLastCheckedNow()
{
    AppSettings settings;
    settings.setValue( QLatin1String( kLastCheckedKey ),
                      QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );
}

QNetworkAccessManager* UpdateService::networkManager()
{
    // 지연 생성. 첫 점검이 기동 몇 초 뒤이므로 startup 에 영향이 없다.
    if( network_ == nullptr )
        network_ = new QNetworkAccessManager( this );
    return network_;
}

void UpdateService::abortReply()
{
    if( reply_ == nullptr )
        return;
    reply_->disconnect( this );
    reply_->abort();
    reply_->deleteLater();
    reply_.clear();
}

// ── 점검 ──────────────────────────────────────────────────

void UpdateService::checkAsync( const bool userInitiated )
{
    if( isBusy() )
    {
        if( userInitiated )
            log( tr( "이미 진행 중입니다 (%1)." ).arg( phaseText() ) );
        return;
    }

    // 이미 설치 준비가 끝나 있으면 다시 확인할 이유가 없다.
    if( state_ == State::ReadyToInstall )
    {
        if( userInitiated )
            emit readyToInstall( info_ );
        return;
    }

    userInitiated_ = userInitiated;
    lastError_.clear();
    setState( State::Checking );

    const QUrl url = withCacheBuster( manifestUrl(),
                                      QDateTime::currentSecsSinceEpoch() );
    QNetworkRequest request( url );
    request.setHeader( QNetworkRequest::UserAgentHeader, userAgent() );
    // GitHub 은 releases/latest/download/... 를 CDN 으로 302 한다. Qt 6 의
    // 기본값도 이 정책이지만, 기본값이 바뀌어도 동작하도록 못박아 둔다.
    request.setAttribute( QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy );
    request.setAttribute( QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork );
    request.setTransferTimeout( kTransferTimeoutMs );

    log( tr( "새 버전을 확인합니다: %1" ).arg( manifestUrl().toString() ) );
    reply_ = networkManager()->get( request );
    connect( reply_, &QNetworkReply::finished, this, &UpdateService::onManifestFinished );
}

void UpdateService::onManifestFinished()
{
    if( reply_ == nullptr )
        return;

    QNetworkReply* reply = reply_;
    reply_.clear();
    reply->deleteLater();

    const int status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();

    if( reply->error() != QNetworkReply::NoError )
    {
        // 정식 릴리스가 하나도 없으면 latest/download 는 404 다. 이것은 오류가
        // 아니라 "아직 배포된 것이 없다" 는 뜻이므로 조용히 넘어간다.
        if( status == 404 )
        {
            log( tr( "아직 배포된 릴리스가 없습니다." ) );
            writeLastCheckedNow();
            setState( State::Idle );
            emit upToDate( userInitiated_ );
            return;
        }
        fail( tr( "업데이트 확인에 실패했습니다: %1" ).arg( reply->errorString() ) );
        return;
    }

    const QByteArray body = reply->readAll();
    if( body.size() > kMaxManifestBytes )
    {
        fail( tr( "업데이트 정보 파일이 비정상적으로 큽니다 (%1바이트)." ).arg( body.size() ) );
        return;
    }

    QString error;
    // 기대하는 product 는 배포 파일 이름이 아니라 **와이어 계약**이다. 두 값을 같은
    // 것으로 두면 실행 파일 이름을 바꾸는 순간 자기 매니페스트도 거부한다.
    const UpdateInfo info = parseUpdateManifest( body, QStringLiteral( MRST_UPDATE_PRODUCT_ID ), &error,
                                                 manifestUrl().host() );
    if( !info.isValid() )
    {
        fail( error.isEmpty() ? tr( "업데이트 정보를 해석할 수 없습니다." ) : error );
        return;
    }

    writeLastCheckedNow();
    info_ = info;

    const QString current = QCoreApplication::applicationVersion();
    log( tr( "설치된 버전 %1, 배포된 버전 %2" ).arg( current, info.version ) );

    if( !info.isNewerThan( current ) )
    {
        // 이미 최신이다. 지난 설치가 남긴 작업 파일이 있으면 이 기회에 치운다.
        setState( State::Idle );
        emit upToDate( userInitiated_ );
        return;
    }

    if( !info.isUpgradableFrom( current ) )
    {
        fail( tr( "이 버전(%1)에서는 자동 업데이트를 할 수 없습니다. "
                  "릴리스 페이지에서 전체 패키지를 받아 주세요." ).arg( current ) );
        return;
    }

    if( !userInitiated_ && info.isSkipped( skippedVersion() ) )
    {
        log( tr( "%1 은 건너뛰기로 지정된 버전입니다." ).arg( info.version ) );
        setState( State::Idle );
        return;
    }

    // 지난번에 받아 둔 것이 그대로 남아 있으면 다시 받지 않는다.
    if( restoreStagedIfUsable( current ) )
        return;

    setState( State::UpdateAvailable );
    emit updateFound( info_ );
}

// ── 다운로드 ──────────────────────────────────────────────

void UpdateService::downloadAsync()
{
    if( state_ != State::UpdateAvailable )
        return;
    if( !info_.isValid() )
        return;

    // 사용자가 직접 [내려받기] 를 누른 동작이다. 이후 실패는 조용히 묻지 않고 알린다.
    userInitiated_ = true;

    if( !installer_->isAppDirectoryWritable() )
    {
        fail( tr( "설치 폴더에 쓸 수 없어 자동 업데이트를 할 수 없습니다.\n%1\n"
                  "릴리스 페이지에서 직접 받아 주세요." ).arg( UpdateInstaller::appDirectory() ) );
        return;
    }

    // 작업 디렉터리를 먼저 만든다. 락 파일이 그 안에 있으므로 순서를 바꾸면
    // 디렉터리가 없어서 락 파일을 못 만들고, 그것이 "다른 인스턴스가 쓰는 중"
    // 으로 잘못 보고된다.
    QString error;
    if( !installer_->prepareWorkspace( &error ) )
    {
        fail( error );
        return;
    }

    if( !installer_->acquireLock() )
    {
        fail( tr( "다른 인스턴스가 업데이트를 진행하고 있습니다." ) );
        return;
    }

    if( !installer_->resetStaging( &error ) )
    {
        installer_->releaseLock();
        fail( error );
        return;
    }

    // 내려받을 공간 + 풀어 놓을 공간 + 구버전을 backup 으로 밀어 둘 공간이
    // 필요하다. 중간에 디스크가 차서 반쯤 교체된 상태가 되는 것이 최악이므로
    // 시작 전에 막는다.
    const qint64 required = info_.asset.size * ( 1 + 2 * kEstimatedExpansionFactor );
    const QStorageInfo storage( installer_->workRoot() );
    if( storage.isValid() && storage.bytesAvailable() < required )
    {
        installer_->releaseLock();
        fail( tr( "디스크 여유 공간이 부족합니다. 약 %1GB 가 필요합니다." )
                 .arg( required / 1073741824.0, 0, 'f', 1 ) );
        return;
    }

    // 파일 이름은 매니페스트가 따로 알려 주지 않고 URL 마지막 조각에서 얻는다.
    zipPath_ = installer_->downloadPath( info_.asset.url.fileName() );
    zipFile_ = std::make_unique< QSaveFile >( zipPath_ );
    // QSaveFile 이라 중간에 끊기면 반쪽 파일이 남지 않는다.
    if( !zipFile_->open( QIODevice::WriteOnly ) )
    {
        const QString path = zipPath_;
        zipFile_.reset();
        installer_->releaseLock();
        fail( tr( "임시 파일을 만들 수 없습니다: %1" ).arg( path ) );
        return;
    }

    setState( State::Downloading );
    emit progressChanged( 0, phaseText() );

    QNetworkRequest request( info_.asset.url );
    request.setHeader( QNetworkRequest::UserAgentHeader, userAgent() );
    request.setAttribute( QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy );
    request.setTransferTimeout( kTransferTimeoutMs );

    hash_.reset();
    log( tr( "%1 을 내려받습니다 (%2MB)." )
            .arg( info_.version )
            .arg( info_.asset.size / 1048576.0, 0, 'f', 0 ) );

    reply_ = networkManager()->get( request );

    connect( reply_, &QNetworkReply::readyRead, this, [ this ] {
        if( reply_ == nullptr || zipFile_ == nullptr )
            return;
        // 메모리에 담지 않는다. 해시도 같은 패스에서 계산해 파일을 두 번 읽지 않는다.
        const QByteArray chunk = reply_->readAll();
        hash_.addData( chunk );
        if( zipFile_->write( chunk ) != chunk.size() )
            reply_->abort();       // 디스크 오류. finished 에서 처리한다
    } );
    connect( reply_, &QNetworkReply::downloadProgress, this,
            [ this ]( const qint64 received, const qint64 total ) {
                // total 은 서버가 Content-Length 를 주지 않으면 -1 이다. 그때는
                // 불확정(-1)으로 넘겨 UI 가 퍼센트를 감추게 한다.
                int percent = -1;
                if( total > 0 )
                    percent = static_cast< int >( qMin< qint64 >( received * 100 / total, 100 ) );
                emit progressChanged( percent, phaseText() );
            } );
    connect( reply_, &QNetworkReply::finished, this, &UpdateService::onDownloadFinished );
}

void UpdateService::onDownloadFinished()
{
    if( reply_ == nullptr )
        return;

    QNetworkReply* reply = reply_;
    reply_.clear();
    reply->deleteLater();

    const bool cancelled = ( reply->error() == QNetworkReply::OperationCanceledError );

    if( reply->error() != QNetworkReply::NoError )
    {
        if( zipFile_ != nullptr )
        {
            zipFile_->cancelWriting();
            zipFile_.reset();
        }
        installer_->releaseLock();

        if( cancelled )
        {
            log( tr( "내려받기를 취소했습니다." ) );
            setState( State::Idle );
            return;
        }
        fail( tr( "내려받기에 실패했습니다: %1" ).arg( reply->errorString() ) );
        return;
    }

    if( zipFile_ == nullptr || !zipFile_->commit() )
    {
        zipFile_.reset();
        installer_->releaseLock();
        fail( tr( "내려받은 파일을 저장할 수 없습니다: %1" ).arg( zipPath_ ) );
        return;
    }
    zipFile_.reset();

    const QString digest = QString::fromLatin1( hash_.result().toHex() );
    if( digest.compare( info_.asset.sha256, Qt::CaseInsensitive ) != 0 )
    {
        // 압축을 풀기 전에 걸러 낸다. 여기서 통과하지 못한 파일은 남겨 둘 이유가 없다.
        QFile::remove( zipPath_ );
        installer_->releaseLock();
        fail( tr( "내려받은 파일이 손상되었습니다 (검사값 불일치). 다시 시도해 주세요." ) );
        return;
    }

    log( tr( "검사값이 일치합니다. 압축을 풉니다." ) );
    startExtract();
}

// ── 압축 해제 ─────────────────────────────────────────────

void UpdateService::startExtract()
{
    const QString tar = systemBsdTarPath();
    if( tar.isEmpty() )
    {
        installer_->releaseLock();
        fail( tr( "이 Windows 에는 zip 을 풀 수 있는 tar.exe 가 없습니다. "
                  "릴리스 페이지에서 직접 받아 주세요." ) );
        return;
    }

    setState( State::Extracting );
    extractedEntries_ = 0;
    emit progressChanged( 0, phaseText() );

    UvTask::Request request;
    request.program = tar;
    request.arguments = {
        QStringLiteral( "-x" ),
        // 엔트리마다 한 줄을 내므로 매니페스트의 entryCount 와 함께 진행률이 된다.
        QStringLiteral( "-v" ),
        QStringLiteral( "-f" ), QDir::toNativeSeparators( zipPath_ ),
        // ZIP 최상위는 사람이 손으로 풀 때를 위해 버전 폴더로 한 겹 감싸 두었다.
        // staging 에는 그 안쪽만 들어가야 한다.
        QStringLiteral( "--strip-components=1" ),
        QStringLiteral( "-C" ), QDir::toNativeSeparators( installer_->stagingDirectory() ),
    };
    // -P 는 절대 주지 않는다. 기본 동작이 '..' 와 절대 경로를 떼어 내는 유일한
    // zip-slip 방어선이다.
    request.mergeChannels = true;
    request.tag           = QStringLiteral( "tar -x (update)" );

    auto* task     = new UvTask( std::move( request ), this );
    extractTask_   = task;

    connect( task, &UvTask::outputLine, this, [ this ]( const QString& ) {
        ++extractedEntries_;
        if( info_.asset.entryCount > 0 )
        {
            emit progressChanged(
                qBound( 0, extractedEntries_ * 100 / info_.asset.entryCount, 99 ), phaseText() );
        }
    } );
    connect( task, &UvTask::failedToStart, this, [ this, task ]( const QString& message ) {
        extractTask_.clear();
        task->deleteLater();
        installer_->releaseLock();
        fail( tr( "압축을 풀 수 없습니다: %1" ).arg( message ) );
    } );
    connect( task, &UvTask::finished, this, [ this, task ]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        const QString output = task->collectedOutput();
        extractTask_.clear();
        task->deleteLater();

        if( cancelled )
        {
            // 반쯤 풀린 staging 과 150MB ZIP 을 남겨 둘 이유가 없다.
            installer_->removePathsAsync( { installer_->stagingDirectory(), zipPath_ } );
            installer_->releaseLock();
            log( tr( "압축 풀기를 취소했습니다." ) );
            setState( State::Idle );
            return;
        }
        if( crashed || exitCode != 0 )
        {
            // tar 는 사람이 읽을 수 있는 오류 문장을 낸다. 그대로 로그에 남긴다.
            if( !output.isEmpty() )
                emit logMessage( output );
            installer_->releaseLock();
            fail( tr( "압축 풀기에 실패했습니다 (종료 코드 %1)." ).arg( exitCode ) );
            return;
        }

        startVerification();
    } );

    task->start();
}

// ── 검증 ──────────────────────────────────────────────────

void UpdateService::startVerification()
{
    setState( State::Verifying );
    emit progressChanged( -1, phaseText() );

    const QDir staging( installer_->stagingDirectory() );
    // 실행 중 프로세스 이름이 아니라 **이 빌드가 배포하는 이름**을 본다. 실행 이름을
    // 기준으로 삼으면, 이름이 바뀐 배포본을 받은 구버전이 "자기 이름" 을 스테이징에서
    // 찾다가 166MB 를 받아 놓고 검증에서 실패한다. 세대가 갈리는 업데이트는 매니페스트
    // product 로 애초에 막는다(cmake/MrstNames.cmake).
    const QString appName     = QStringLiteral( MRST_APP_EXE_NAME );
    const QString updaterName = QStringLiteral( MRST_UPDATER_EXE_NAME );
    const QString appPath     = staging.filePath( appName );
    const QString updaterPath = staging.filePath( updaterName );
    const QString expected    = info_.version;

    // 90MB exe 의 Authenticode 검증은 파일 전체를 해시한다. GUI 스레드에서
    // 하면 수백 ms 멈추므로 작업 스레드로 보낸다 (PythonEnvManager 의 대용량
    // 복사와 같은 관용구).
    QPointer< UpdateService > guard( this );
    QThreadPool::globalInstance()->start( [ guard, appPath, updaterPath, expected ] {
        QString message;
        bool ok = true;

        for( const QString& path : { appPath, updaterPath } )
        {
            // 90MB exe 전체를 해시한다. verifyAuthenticode() 안(WinVerifyTrust)은
            // 취소할 수 없으므로, 최소한 두 번째 파일로 넘어가지는 않게 한다.
            if( isShuttingDown() )
                return;

            if( !QFileInfo::exists( path ) )
            {
                message = UpdateService::tr( "배포 파일에 %1 이 없습니다." )
                             .arg( QFileInfo( path ).fileName() );
                ok = false;
                break;
            }

            const SignatureInfo signature = verifyAuthenticode( path );
            if( !isTrustedPublisher( signature ) )
            {
                message = UpdateService::tr( "%1 의 서명을 확인할 수 없습니다. %2" )
                             .arg( QFileInfo( path ).fileName(),
                                   signature.trusted
                                       ? UpdateService::tr( "서명자: %1" ).arg( signature.subject )
                                       : signature.errorText );
                ok = false;
                break;
            }
        }

        if( ok )
        {
            // 태그와 에셋이 어긋난 릴리스를 잡는다.
            const QString actual = fileVersionString( appPath );
            if( !actual.isEmpty() && compareVersions( actual, expected ) != 0 )
            {
                message = UpdateService::tr( "배포 파일의 버전(%1)이 업데이트 정보(%2)와 다릅니다." )
                             .arg( actual, expected );
                ok = false;
            }
        }

        QMetaObject::invokeMethod( guard, [ guard, ok, message ] {
            if( guard )
                guard->onVerified( ok, message );
        }, Qt::QueuedConnection );
    } );
}

void UpdateService::onVerified( const bool ok, const QString& message )
{
    if( !ok )
    {
        // 검증에 실패한 staging 은 남겨 두면 다음 기동 때 다시 설치를 시도하게 된다.
        installer_->clearStagedUpdate();
        installer_->removePathsAsync( { installer_->stagingDirectory(), zipPath_ } );
        installer_->releaseLock();
        fail( message.isEmpty() ? tr( "배포 파일을 확인할 수 없습니다." ) : message );
        return;
    }

    StagedUpdate staged;
    staged.version  = info_.version;
    staged.removals = info_.removals;

    QString error;
    if( !installer_->writeStagedUpdate( staged, &error ) )
    {
        installer_->releaseLock();
        fail( error );
        return;
    }

    // ZIP 은 더 필요하지 않다. 150MB 를 붙들고 있을 이유가 없다.
    installer_->removePathsAsync( { zipPath_ } );
    installer_->releaseLock();

    log( tr( "%1 설치 준비가 끝났습니다." ).arg( info_.version ) );
    setState( State::ReadyToInstall );
    emit progressChanged( 100, phaseText() );
    emit readyToInstall( info_ );
}

// ── 취소 ──────────────────────────────────────────────────

void UpdateService::cancel()
{
    switch( state_ )
    {
        case State::Checking:
        case State::Downloading:
            abortReply();
            if( zipFile_ != nullptr )
            {
                zipFile_->cancelWriting();
                zipFile_.reset();
            }
            if( !zipPath_.isEmpty() )
                installer_->removePathsAsync( { zipPath_ } );
            installer_->releaseLock();
            log( tr( "취소했습니다." ) );
            setState( State::Idle );
            return;

        case State::Extracting:
            if( extractTask_ != nullptr )
                extractTask_->cancel();   // finished 핸들러가 상태를 되돌린다
            return;

        default:
            // 검증과 설치는 중간에 끊으면 오히려 위험하다. 그냥 끝까지 간다.
            return;
    }
}

// ── 설치 ──────────────────────────────────────────────────

bool UpdateService::launchInstaller()
{
    if( state_ != State::ReadyToInstall )
        return false;

    QString error;
    if( !installer_->launchUpdater( /*relaunchApp=*/true, &error ) )
    {
        // staging 은 남겨 둔다. 다음 실행에서 다시 설치할 수 있다.
        log( error );
        emit failed( error, /*silent=*/false );
        return false;
    }

    setState( State::Installing );
    return true;
}

// ── 기동 시 정리 ──────────────────────────────────────────

bool UpdateService::restoreStagedIfUsable( const QString& currentVersion )
{
    const StagedUpdate staged = installer_->stagedUpdate();
    if( !staged.isValid() )
        return false;

    // 이미 설치가 끝난 뒤라면(= 준비된 버전이 지금 버전보다 높지 않다) 정리한다.
    // 여기가 backup 을 커밋하는 지점이다 — 새 버전이 한 번 정상 기동했으므로
    // 되돌릴 필요가 없어졌다. staging 이 비어 있는 것은 교체가 끝났다는 뜻이지
    // 준비물이 없다는 뜻이 아니다(그렇게 읽으면 백업 400MB 가 영원히 남는다).
    if( compareVersions( staged.version, currentVersion ) <= 0 )
    {
        log( tr( "설치가 완료되어 이전 버전 백업을 정리합니다." ) );
        installer_->clearStagedUpdate();
        installer_->removePathsAsync( { installer_->stagingDirectory(),
                                              installer_->backupDirectory(),
                                              installer_->downloadDirectory() } );
        return false;
    }

    // 더 높은 버전을 준비했다고 적혀 있는데 내용이 없다. 교체가 중간에 끊겼거나
    // 사용자가 staging 을 지운 경우다. 남은 것을 치우고 처음부터 다시 받게 한다.
    if( !staged.payloadPresent )
    {
        log( tr( "준비된 파일이 사라져 업데이트 작업 폴더를 정리합니다." ) );
        installer_->clearStagedUpdate();
        installer_->removePathsAsync( { installer_->stagingDirectory(),
                                              installer_->backupDirectory(),
                                              installer_->downloadDirectory() } );
        return false;
    }

    // 아직 설치하지 않은 준비물이 남아 있다.
    if( info_.version.isEmpty() )
        info_.version = staged.version;
    if( info_.removals.isEmpty() )
        info_.removals = staged.removals;

    log( tr( "%1 설치 준비가 이미 되어 있습니다." ).arg( staged.version ) );
    setState( State::ReadyToInstall );
    emit readyToInstall( info_ );
    return true;
}

void UpdateService::reconcileAfterRestart()
{
    const QString current = QCoreApplication::applicationVersion();

    const InstallOutcome outcome = installer_->takePreviousOutcome();
    if( outcome.attempted )
    {
        if( outcome.succeeded )
            log( tr( "%1 로 업데이트했습니다." ).arg( outcome.version ) );
        else
            log( tr( "업데이트를 적용하지 못했습니다: %1" ).arg( outcome.errorMessage ) );

        emit installOutcomeReported( outcome.succeeded, outcome.version, outcome.errorMessage );
    }

    // %TEMP% 에 남은 업데이터 사본을 치운다. 우리 pid 로 만든 폴더만 본다
    // (다른 인스턴스가 지금 쓰고 있을 수 있다).
    const QString tempRoot = QDir( QStandardPaths::writableLocation( QStandardPaths::TempLocation ) )
                                .filePath( QStringLiteral( "mrst-updater-%1" )
                                              .arg( QCoreApplication::applicationPid() ) );
    if( QFileInfo::exists( tempRoot ) )
        installer_->removePathsAsync( { tempRoot } );

    restoreStagedIfUsable( current );
}

}  // namespace mrst
