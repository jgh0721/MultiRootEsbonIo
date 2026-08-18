#include "stdafx.h"
#include "core/solUpdateInstaller.hpp"

#include "mrst_version.h"

#include "utils/ProcessReaper.hpp"
#include "utils/solBackgroundWork.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThreadPool>

namespace mrst {

namespace {

constexpr auto kWorkDirName     = ".update";
constexpr auto kDownloadDirName = "download";
constexpr auto kStagingDirName  = "staging";
constexpr auto kBackupDirName   = "backup";
constexpr auto kUpdaterLogName  = "updater.log";
constexpr auto kResultName      = "result.ini";
constexpr auto kStagedName      = "staged.ini";
constexpr auto kLockName        = "lock";
constexpr auto kUpdaterFileName = MRST_UPDATER_EXE_NAME;

bool ensureDirectory( const QString& path, QString* errorMessage )
{
    if( QDir().mkpath( path ) )
        return true;
    if( errorMessage != nullptr )
        // 익명 네임스페이스의 자유 함수라 tr() 이 없다. UpdateInstaller 가 이
        // 문구를 그대로 사용자에게 보여 주므로 컨텍스트를 그쪽에 맞춘다.
        *errorMessage = QCoreApplication::translate( "mrst::UpdateInstaller",
                                                     "디렉터리를 만들 수 없습니다: %1" )
                            .arg( path );
    return false;
}

}  // namespace

UpdateInstaller::UpdateInstaller( QObject* parent )
    : QObject( parent )
{
}

UpdateInstaller::~UpdateInstaller()
{
    releaseLock();
}

// ── 경로 ──────────────────────────────────────────────────

QString UpdateInstaller::appDirectory()
{
    return QCoreApplication::applicationDirPath();
}

QString UpdateInstaller::applicationExecutable()
{
    return QCoreApplication::applicationFilePath();
}

QString UpdateInstaller::updaterExecutable()
{
    return QDir( appDirectory() ).filePath( QLatin1String( kUpdaterFileName ) );
}

QString UpdateInstaller::workRoot() const
{
    return QDir( appDirectory() ).filePath( QLatin1String( kWorkDirName ) );
}

QString UpdateInstaller::downloadDirectory() const
{
    return QDir( workRoot() ).filePath( QLatin1String( kDownloadDirName ) );
}

QString UpdateInstaller::stagingDirectory() const
{
    return QDir( workRoot() ).filePath( QLatin1String( kStagingDirName ) );
}

QString UpdateInstaller::backupDirectory() const
{
    return QDir( workRoot() ).filePath( QLatin1String( kBackupDirName ) );
}

QString UpdateInstaller::updaterLogPath() const
{
    return QDir( workRoot() ).filePath( QLatin1String( kUpdaterLogName ) );
}

QString UpdateInstaller::resultPath() const
{
    return QDir( workRoot() ).filePath( QLatin1String( kResultName ) );
}

QString UpdateInstaller::stagedInfoPath() const
{
    return QDir( workRoot() ).filePath( QLatin1String( kStagedName ) );
}

QString UpdateInstaller::lockPath() const
{
    return QDir( workRoot() ).filePath( QLatin1String( kLockName ) );
}

QString UpdateInstaller::downloadPath( const QString& assetName ) const
{
    // 매니페스트가 준 이름을 그대로 경로에 붙이지 않는다. 파일명 부분만 쓴다.
    const QString safeName = QFileInfo( assetName ).fileName();
    return QDir( downloadDirectory() )
        .filePath( safeName.isEmpty() ? QStringLiteral( "update.zip" ) : safeName );
}

// ── 쓰기 권한 ─────────────────────────────────────────────

bool UpdateInstaller::isAppDirectoryWritable() const
{
    QTemporaryFile probe( QDir( appDirectory() ).filePath( QStringLiteral( ".mrst-write-probe-XXXXXX" ) ) );
    probe.setAutoRemove( true );
    return probe.open();
}

// ── 작업 공간 ─────────────────────────────────────────────

bool UpdateInstaller::prepareWorkspace( QString* errorMessage )
{
    return ensureDirectory( workRoot(), errorMessage )
        && ensureDirectory( downloadDirectory(), errorMessage )
        && ensureDirectory( stagingDirectory(), errorMessage )
        && ensureDirectory( backupDirectory(), errorMessage );
}

bool UpdateInstaller::resetStaging( QString* errorMessage )
{
    QDir staging( stagingDirectory() );
    if( staging.exists() && !staging.removeRecursively() )
    {
        if( errorMessage != nullptr )
        {
            *errorMessage = tr( "이전 준비 파일을 지울 수 없습니다: %1" )
                               .arg( stagingDirectory() );
        }
        return false;
    }
    return ensureDirectory( stagingDirectory(), errorMessage );
}

// ── 락 ────────────────────────────────────────────────────

bool UpdateInstaller::acquireLock()
{
    if( lockHeld_ )
        return true;

    const QString path = lockPath();
    QFile file( path );
    if( file.exists() && file.open( QIODevice::ReadOnly | QIODevice::Text ) )
    {
        const qint64 pid = file.readAll().trimmed().toLongLong();
        file.close();

        // 우리 자신이 남긴 락이면 그대로 이어 쓴다.
        if( pid != 0 && pid != QCoreApplication::applicationPid() && isProcessRunning( pid ) )
        {
            emit logMessage( tr( "[업데이트] 다른 인스턴스(pid %1)가 업데이트 중입니다." )
                                .arg( pid ) );
            return false;
        }
        // 크래시로 남은 락이다. 덮어쓴다.
    }

    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text ) )
    {
        // 여기로 오는 것은 대개 작업 디렉터리를 아직 만들지 않았다는 뜻이다.
        // 호출자가 원인을 알 수 있게 남긴다 ("다른 인스턴스" 로 오해하기 쉽다).
        emit logMessage( tr( "[업데이트] 잠금 파일을 만들 수 없습니다: %1" ).arg( path ) );
        return false;
    }

    file.write( QByteArray::number( QCoreApplication::applicationPid() ) );
    file.close();
    lockHeld_ = true;
    return true;
}

void UpdateInstaller::releaseLock()
{
    if( !lockHeld_ )
        return;
    QFile::remove( lockPath() );
    lockHeld_ = false;
}

// ── staging 정보 ──────────────────────────────────────────

StagedUpdate UpdateInstaller::stagedUpdate() const
{
    StagedUpdate staged;

    const QString path = stagedInfoPath();
    if( !QFile::exists( path ) )
        return staged;

    const QSettings info( path, QSettings::IniFormat );
    staged.version  = info.value( QStringLiteral( "version" ) ).toString();
    staged.removals = info.value( QStringLiteral( "removals" ) ).toStringList();

    // 교체가 끝났으면 staging 은 비어 있다. 그것도 의미 있는 상태이므로 버리지
    // 않고 그대로 알린다 (호출자가 백업을 정리할 시점을 여기서 판단한다).
    // 실행 중 이름이 아니라 배포 이름으로 본다 (UpdateService::startVerification 과 같은 이유).
    staged.payloadPresent = QFileInfo::exists(
        QDir( stagingDirectory() ).filePath( QStringLiteral( MRST_APP_EXE_NAME ) ) );

    return staged;
}

bool UpdateInstaller::writeStagedUpdate( const StagedUpdate& staged, QString* errorMessage )
{
    QSettings info( stagedInfoPath(), QSettings::IniFormat );
    info.setValue( QStringLiteral( "version" ), staged.version );
    info.setValue( QStringLiteral( "removals" ), staged.removals );
    info.setValue( QStringLiteral( "stagedAt" ),
                  QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );
    info.sync();

    if( info.status() == QSettings::NoError )
        return true;

    if( errorMessage != nullptr )
        *errorMessage = tr( "준비 정보를 저장할 수 없습니다: %1" ).arg( stagedInfoPath() );
    return false;
}

void UpdateInstaller::clearStagedUpdate()
{
    QFile::remove( stagedInfoPath() );
}

// ── 업데이터 실행 ─────────────────────────────────────────

bool UpdateInstaller::launchUpdater( const bool relaunchApp, QString* errorMessage )
{
    const StagedUpdate staged = stagedUpdate();
    // 정보만 있고 내용이 없으면 띄워도 할 일이 없다 (업데이터가 앱만 죽인다).
    if( !staged.isValid() || !staged.payloadPresent )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "설치할 준비가 된 파일이 없습니다." );
        return false;
    }

    const QString source = updaterExecutable();
    if( !QFileInfo::exists( source ) )
    {
        if( errorMessage != nullptr )
        {
            *errorMessage = tr( "업데이터를 찾을 수 없습니다: %1" ).arg( source );
        }
        return false;
    }

    // 업데이터 자신도 교체 대상이다. 원본을 실행하면 자기 파일을 잠그므로
    // %TEMP% 로 복사해서 그 사본을 돌린다.
    const QString tempRoot = QDir( QStandardPaths::writableLocation( QStandardPaths::TempLocation ) )
                                .filePath( QStringLiteral( "mrst-updater-%1" )
                                              .arg( QCoreApplication::applicationPid() ) );
    if( !ensureDirectory( tempRoot, errorMessage ) )
        return false;

    const QString target = QDir( tempRoot ).filePath( QLatin1String( kUpdaterFileName ) );
    QFile::remove( target );
    if( !QFile::copy( source, target ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "업데이터를 임시 폴더로 복사할 수 없습니다: %1" ).arg( target );
        return false;
    }

    QStringList arguments{
        QStringLiteral( "--pid" ),     QString::number( QCoreApplication::applicationPid() ),
        QStringLiteral( "--target" ),  QDir::toNativeSeparators( appDirectory() ),
        QStringLiteral( "--staging" ), QDir::toNativeSeparators( stagingDirectory() ),
        QStringLiteral( "--backup" ),  QDir::toNativeSeparators( backupDirectory() ),
        QStringLiteral( "--log" ),     QDir::toNativeSeparators( updaterLogPath() ),
        QStringLiteral( "--result" ),  QDir::toNativeSeparators( resultPath() ),
        QStringLiteral( "--version" ), staged.version,
    };
    for( const QString& name : staged.removals )
        arguments << QStringLiteral( "--remove" ) << name;
    if( relaunchApp )
        arguments << QStringLiteral( "--relaunch" ) << QDir::toNativeSeparators( applicationExecutable() );

    // startDetached 로 띄운다. 이 프로세스는 Job Object 에 속하지 않으므로
    // (ProcessReaper 는 자식 pid 만 Job 에 넣는다) 앱이 죽어도 살아남는다.
    // 반대로 assignToKillOnExitJob() 을 부르면 앱 종료와 함께 커널이 죽인다.
    qint64 updaterPid = 0;
    if( !QProcess::startDetached( target, arguments, tempRoot, &updaterPid ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "업데이터를 시작할 수 없습니다: %1" ).arg( target );
        return false;
    }

    emit logMessage( tr( "[업데이트] 업데이터를 시작했습니다 (pid %1). 앱을 닫습니다." )
                        .arg( updaterPid ) );
    return true;
}

// ── 결과 회수 ─────────────────────────────────────────────

QString UpdateInstaller::describeFailure( const QString& id, const QString& detail )
{
    // 업데이터는 Qt 를 링크하지 않아 스스로 번역할 수 없다. 그래서 무엇이
    // 실패했는지만 식별자로 남기고, 문장은 여기서 사용자의 언어로 만든다.
    // (detail 은 Windows 가 준 설명이라 이미 OS 언어로 되어 있다.)
    const auto withDetail = [ &detail ]( const QString& sentence ) {
        return detail.isEmpty() ? sentence
                                : QStringLiteral( "%1 (%2)" ).arg( sentence, detail );
    };

    if( id == QLatin1String( "updater.appStillAlive" ) )
        return tr( "앱이 종료되지 않아 업데이트를 적용하지 못했습니다." );
    if( id == QLatin1String( "updater.noStaging" ) )
        return tr( "준비된 파일을 찾을 수 없습니다." );
    if( id == QLatin1String( "updater.stagingEmpty" ) )
        return tr( "준비된 파일이 없습니다." );
    if( id == QLatin1String( "updater.backupDirty" ) )
        return tr( "이전 백업을 정리할 수 없습니다." );
    if( id == QLatin1String( "updater.backupCreateFailed" ) )
        return withDetail( tr( "백업 폴더를 만들 수 없습니다." ) );
    if( id == QLatin1String( "updater.noBackup" ) )
        return tr( "되돌릴 백업이 없습니다." );
    if( id == QLatin1String( "updater.backupEmpty" ) )
        return tr( "백업 폴더가 비어 있습니다." );
    if( id == QLatin1String( "updater.swapFailed" ) )
        return withDetail( tr( "파일을 교체하지 못했습니다." ) );

    // 모르는 값이다. 교체 직전에 도는 업데이터는 **항상 구버전**이라, 예전
    // 형식(완성된 한국어 문장)이 계속 들어온다. 그대로 보여 준다.
    return id;
}

InstallOutcome UpdateInstaller::takePreviousOutcome()
{
    InstallOutcome outcome;

    const QString path = resultPath();
    if( !QFile::exists( path ) )
        return outcome;

    {
        const QSettings result( path, QSettings::IniFormat );
        outcome.attempted    = true;
        outcome.succeeded    = result.value( QStringLiteral( "succeeded" ), false ).toBool();
        outcome.version      = result.value( QStringLiteral( "version" ) ).toString();
        outcome.errorMessage = describeFailure( result.value( QStringLiteral( "error" ) ).toString(),
                                                result.value( QStringLiteral( "errorDetail" ) ).toString() );
    }

    // 한 번만 보고한다. 지우지 못해도(다른 프로세스가 열고 있다면) 흐름을
    // 막지는 않는다.
    QFile::remove( path );
    return outcome;
}

void UpdateInstaller::removePathsAsync( const QStringList& paths )
{
    if( paths.isEmpty() )
        return;

    QPointer< UpdateInstaller > guard( this );
    QThreadPool::globalInstance()->start( [ guard, paths ] {
        QStringList failed;
        for( const QString& path : paths )
        {
            // staging/backup 은 150~400MB 다. 종료 중이면 그만둔다 — 중간에
            // 끊겨도 다음 기동의 restoreStagedIfUsable() 이 마저 지운다.
            if( isShuttingDown() )
                return;

            if( path.isEmpty() || !QFileInfo::exists( path ) )
                continue;

            QFileInfo info( path );
            const bool ok = info.isDir() ? QDir( path ).removeRecursively() : QFile::remove( path );
            if( !ok )
                failed.append( path );
        }

        if( failed.isEmpty() )
            return;

        QMetaObject::invokeMethod( guard, [ guard, failed ] {
            if( guard )
            {
                emit guard->logMessage( tr( "[업데이트] 정리하지 못한 항목: %1" )
                                           .arg( failed.join( QStringLiteral( ", " ) ) ) );
            }
        }, Qt::QueuedConnection );
    } );
}

}  // namespace mrst
