#include "stdafx.h"
#include "core/solPythonEnvMgr.hpp"

#include "core/solAppSettings.hpp"
#include "core/solUvTaskRunner.hpp"

#include <QCryptographicHash>
#include <QSettings>

namespace mrst {
namespace {

constexpr auto kSettingsUseExternalUv = "PythonEnv/useExternalUv";
constexpr auto kSettingsExternalUvPath = "PythonEnv/externalUvPath";
constexpr auto kSettingsAutoBootstrap = "PythonEnv/autoBootstrap";
constexpr auto kSettingsInstallThemes = "PythonEnv/installOptionalThemes";
constexpr auto kSettingsInstallExtensions = "PythonEnv/installOptionalExtensions";
constexpr auto kSettingsExtraPackages = "PythonEnv/extraPackages";

QString nativePath( const QString& path )
{
    return QDir::toNativeSeparators( path );
}

QString scriptExe( const QString& venvDir, const QString& name )
{
#ifdef Q_OS_WIN
    return QDir( venvDir ).filePath( QStringLiteral( "Scripts/%1.exe" ).arg( name ) );
#else
    return QDir( venvDir ).filePath( QStringLiteral( "bin/%1" ).arg( name ) );
#endif
}

QString sha1Of( const QByteArray& bytes )
{
    return QString::fromLatin1( QCryptographicHash::hash( bytes, QCryptographicHash::Sha1 ).toHex() );
}

/// 리소스를 대상 경로로 복사한다. 내용이 같으면 건드리지 않는다.
bool copyResourceFile( const QString& resourcePath, const QString& destinationPath,
                      const bool executable, QString* errorMessage )
{
    QFile source( resourcePath );
    if( !source.open( QIODevice::ReadOnly ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = PythonEnvManager::tr( "리소스를 열 수 없습니다: %1" ).arg( resourcePath );
        return false;
    }
    const QByteArray payload = source.readAll();
    source.close();

    QFile existing( destinationPath );
    if( existing.exists() && existing.open( QIODevice::ReadOnly ) )
    {
        const QByteArray current = existing.readAll();
        existing.close();
        if( current == payload )
            return true;   // 이미 최신
    }

    QDir().mkpath( QFileInfo( destinationPath ).absolutePath() );
    QFile target( destinationPath );
    if( !target.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = PythonEnvManager::tr( "파일을 쓸 수 없습니다: %1" ).arg( nativePath( destinationPath ) );
        return false;
    }
    target.write( payload );
    target.close();

    if( executable )
        target.setPermissions( target.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser );

    return true;
}

/// 워커 스레드에서 수행할 리소스 추출에 필요한 값 묶음.
/// (스레드에서 PythonEnvManager 멤버를 건드리지 않기 위해 값으로 넘긴다.)
struct ExtractPlan
{
    QString runtimeRoot;
    QString projectDir;
    QString uvTarget;
    bool    useExternalUv = false;
    QString externalUvPath;
};

bool runExtractPlan( const ExtractPlan& plan, QString* errorMessage )
{
    if( !QDir().mkpath( plan.runtimeRoot ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = PythonEnvManager::tr( "Environment 디렉터리를 만들 수 없습니다: %1" )
                                .arg( nativePath( plan.runtimeRoot ) );
        return false;
    }

    if( !copyResourceFile( QStringLiteral( ":/python/pyproject.toml" ),
                          QDir( plan.projectDir ).filePath( QStringLiteral( "pyproject.toml" ) ), false, errorMessage ) )
        return false;

    // uv.lock 이 있어야 --frozen 으로 재현 가능한 설치가 된다.
    if( !copyResourceFile( QStringLiteral( ":/python/uv.lock" ),
                          QDir( plan.projectDir ).filePath( QStringLiteral( "uv.lock" ) ), false, errorMessage ) )
        return false;

    QFile pythonVersion( QDir( plan.projectDir ).filePath( QStringLiteral( ".python-version" ) ) );
    if( !pythonVersion.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = PythonEnvManager::tr( ".python-version 파일을 쓸 수 없습니다: %1" )
                                .arg( nativePath( pythonVersion.fileName() ) );
        return false;
    }
    pythonVersion.write( "3.12\n" );
    pythonVersion.close();

    if( plan.useExternalUv )
    {
        if( plan.externalUvPath.isEmpty() || !QFileInfo::exists( plan.externalUvPath ) )
        {
            if( errorMessage != nullptr )
                *errorMessage = PythonEnvManager::tr( "외부 UV 파일을 찾을 수 없습니다: %1" )
                                    .arg( nativePath( plan.externalUvPath ) );
            return false;
        }
        return true;
    }

    return copyResourceFile( QStringLiteral( ":/python/uv.exe" ), plan.uvTarget, true, errorMessage );
}

}  // namespace

PythonEnvManager::PythonEnvManager( QObject* parent )
    : QObject( parent )
{
    AppSettings settings;
    useExternalUv_ = settings.value( kSettingsUseExternalUv, false ).toBool();
    externalUvPath_ = settings.value( kSettingsExternalUvPath ).toString();
    autoBootstrap_ = settings.value( kSettingsAutoBootstrap, true ).toBool();
    installOptionalThemes_ = settings.value( kSettingsInstallThemes, true ).toBool();
    installOptionalExtensions_ = settings.value( kSettingsInstallExtensions, true ).toBool();
    extraPackages_ = settings.value( kSettingsExtraPackages ).toStringList();

    refreshState();
}

PythonEnvManager::~PythonEnvManager() = default;

// ── 경로 ────────────────────────────────────────────────────

QString PythonEnvManager::runtimeRoot() const
{
    return QDir( QCoreApplication::applicationDirPath() ).filePath( QStringLiteral( "Environment" ) );
}

QString PythonEnvManager::projectDir() const
{
    return runtimeRoot();
}

QString PythonEnvManager::venvDir() const
{
    return QDir( projectDir() ).filePath( QStringLiteral( ".venv" ) );
}

QString PythonEnvManager::pythonExe() const
{
#ifdef Q_OS_WIN
    return QDir( venvDir() ).filePath( QStringLiteral( "Scripts/python.exe" ) );
#else
    return QDir( venvDir() ).filePath( QStringLiteral( "bin/python" ) );
#endif
}

QString PythonEnvManager::sphinxBuildExe() const
{
    return scriptExe( venvDir(), QStringLiteral( "sphinx-build" ) );
}

QString PythonEnvManager::esbonioExe() const
{
    return scriptExe( venvDir(), QStringLiteral( "esbonio" ) );
}

QString PythonEnvManager::scriptsDir() const
{
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "scripts" ) );
}

QString PythonEnvManager::cacheDir() const
{
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "cache" ) );
}

QString PythonEnvManager::embeddedUvTarget() const
{
#ifdef Q_OS_WIN
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "uv.exe" ) );
#else
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "uv" ) );
#endif
}

QString PythonEnvManager::readyMarker() const
{
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "ready.marker" ) );
}

QString PythonEnvManager::uvExecutable() const
{
    return useExternalUv_ ? externalUvPath_ : embeddedUvTarget();
}

// ── 상태 ────────────────────────────────────────────────────

EnvState PythonEnvManager::state() const
{
    return state_;
}

bool PythonEnvManager::isReady() const
{
    return state_ == EnvState::Ready;
}

bool PythonEnvManager::isBusy() const
{
    return state_ == EnvState::Preparing || state_ == EnvState::Syncing || state_ == EnvState::Verifying;
}

QString PythonEnvManager::stateText() const
{
    switch( state_ )
    {
        case EnvState::Unknown:        return tr( "확인 전" );
        case EnvState::Checking:       return tr( "확인 중" );
        case EnvState::NotConfigured:  return tr( "환경 없음" );
        case EnvState::Preparing:      return tr( "준비 중" );
        case EnvState::Syncing:        return tr( "패키지 설치 중" );
        case EnvState::Verifying:      return tr( "검증 중" );
        case EnvState::Ready:          return tr( "준비됨" );
        case EnvState::Failed:         return tr( "구성 실패" );
        case EnvState::Cancelled:      return tr( "구성 취소됨" );
    }
    return {};
}

QString PythonEnvManager::lastError() const
{
    return lastError_;
}

QDateTime PythonEnvManager::configuredDate() const
{
    const QFileInfo markerInfo( readyMarker() );
    if( !markerInfo.exists() )
        return {};

    const QDateTime created = markerInfo.birthTime();
    return created.isValid() ? created : markerInfo.lastModified();
}

QString PythonEnvManager::configuredDateText() const
{
    const QDateTime configured = configuredDate();
    if( !configured.isValid() )
        return tr( "구성되지 않음" );
    return configured.toLocalTime().toString( QStringLiteral( "yyyy-MM-dd HH:mm:ss" ) );
}

QString PythonEnvManager::uvDescription() const
{
    const QString uvPath = uvExecutable();

    QStringList lines;
    lines << ( useExternalUv_ ? tr( "외부 UV" ) : tr( "내장 UV" ) );
    lines << tr( "경로: %1" ).arg( nativePath( uvPath ) );
    if( !QFileInfo::exists( uvPath ) )
    {
        lines << ( useExternalUv_ ? tr( "상태: 지정한 UV 파일을 찾을 수 없습니다." )
                                  : tr( "상태: 구성 시 리소스에서 추출됩니다." ) );
    }
    return lines.join( QLatin1Char( '\n' ) );
}

// ── 설정 ────────────────────────────────────────────────────

bool PythonEnvManager::useExternalUv() const           { return useExternalUv_; }
QString PythonEnvManager::externalUvPath() const       { return externalUvPath_; }
bool PythonEnvManager::autoBootstrap() const           { return autoBootstrap_; }
bool PythonEnvManager::installOptionalThemes() const   { return installOptionalThemes_; }
bool PythonEnvManager::installOptionalExtensions() const { return installOptionalExtensions_; }
QStringList PythonEnvManager::extraPackages() const    { return extraPackages_; }

void PythonEnvManager::setUseExternalUv( const bool enabled )
{
    useExternalUv_ = enabled;
}

void PythonEnvManager::setExternalUvPath( const QString& path )
{
    externalUvPath_ = QDir::fromNativeSeparators( path.trimmed() );
}

void PythonEnvManager::setAutoBootstrap( const bool enabled )
{
    autoBootstrap_ = enabled;
}

void PythonEnvManager::setInstallOptionalThemes( const bool enabled )
{
    installOptionalThemes_ = enabled;
}

void PythonEnvManager::setInstallOptionalExtensions( const bool enabled )
{
    installOptionalExtensions_ = enabled;
}

void PythonEnvManager::saveUvSettings() const
{
    AppSettings settings;
    settings.setValue( kSettingsUseExternalUv, useExternalUv_ );
    settings.setValue( kSettingsExternalUvPath, externalUvPath_ );
    settings.setValue( kSettingsAutoBootstrap, autoBootstrap_ );
    settings.setValue( kSettingsInstallThemes, installOptionalThemes_ );
    settings.setValue( kSettingsInstallExtensions, installOptionalExtensions_ );
    settings.setValue( kSettingsExtraPackages, extraPackages_ );
}

// ── 상태 전이 ───────────────────────────────────────────────

void PythonEnvManager::setState( const EnvState next )
{
    if( state_ == next )
        return;

    state_ = next;
    emit stateChanged( state_ );

    const bool ready = isReady();
    if( ready != lastReadyState_ )
    {
        lastReadyState_ = ready;
        emit readyChanged( ready );
    }
}

void PythonEnvManager::setLastError( const QString& message )
{
    lastError_ = message;
}

bool PythonEnvManager::installedFilesPresent() const
{
    return QFileInfo::exists( pythonExe() )
        && QFileInfo::exists( sphinxBuildExe() )
        && QFileInfo::exists( esbonioExe() );
}

QString PythonEnvManager::embeddedPyprojectHash() const
{
    QFile source( QStringLiteral( ":/python/pyproject.toml" ) );
    if( !source.open( QIODevice::ReadOnly ) )
        return {};
    return sha1Of( source.readAll() );
}

bool PythonEnvManager::markerMatchesEmbeddedManifest() const
{
    QSettings marker( readyMarker(), QSettings::IniFormat );
    if( marker.value( QStringLiteral( "schema" ) ).toInt() != RuntimeSchema )
        return false;

    const QString expected = embeddedPyprojectHash();
    return !expected.isEmpty() && marker.value( QStringLiteral( "pyprojectHash" ) ).toString() == expected;
}

void PythonEnvManager::writeReadyMarker() const
{
    QSettings marker( readyMarker(), QSettings::IniFormat );
    marker.setValue( QStringLiteral( "schema" ), RuntimeSchema );
    marker.setValue( QStringLiteral( "pyprojectHash" ), embeddedPyprojectHash() );
    marker.setValue( QStringLiteral( "configuredAt" ), QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );
    marker.sync();
}

void PythonEnvManager::refreshState()
{
    if( isBusy() )
        return;

    setState( EnvState::Checking );

    // 파일만 있는 게 아니라 매니페스트 해시까지 맞아야 준비된 것으로 본다.
    // 앱 업데이트로 의존성이 바뀌면 stale venv 로 조용히 돌지 않고 재동기화한다.
    if( QFileInfo::exists( readyMarker() ) && installedFilesPresent() && markerMatchesEmbeddedManifest() )
        setState( EnvState::Ready );
    else
        setState( EnvState::NotConfigured );
}

// ── 부트스트랩 ──────────────────────────────────────────────

void PythonEnvManager::ensureEnvironmentAsync()
{
    if( isReady() || isBusy() )
        return;

    configureEnvironmentAsync( false );
}

void PythonEnvManager::configureEnvironmentAsync( const bool forceRebuild )
{
    if( isBusy() )
        return;

    setLastError( {} );
    setState( EnvState::Preparing );
    emit progressChanged( -1, tr( "준비 중" ) );
    emit bootstrapLog( tr( "Python/Sphinx/Esbonio 환경 구성을 시작합니다." ) );

    if( forceRebuild && QFileInfo::exists( readyMarker() ) )
        QFile::remove( readyMarker() );

    startExtractTask();
}

void PythonEnvManager::startExtractTask()
{
    ExtractPlan plan;
    plan.runtimeRoot = runtimeRoot();
    plan.projectDir = projectDir();
    plan.uvTarget = embeddedUvTarget();
    plan.useExternalUv = useExternalUv_;
    plan.externalUvPath = externalUvPath_;

    QPointer< PythonEnvManager > guard( this );
    QThreadPool::globalInstance()->start( [guard, plan] {
        QString errorMessage;
        const bool ok = runExtractPlan( plan, &errorMessage );

        QMetaObject::invokeMethod(
            guard,
            [guard, ok, errorMessage] {
                if( guard )
                    guard->onResourcesExtracted( ok, errorMessage );
            },
            Qt::QueuedConnection );
    } );
}

void PythonEnvManager::onResourcesExtracted( const bool ok, const QString& errorMessage )
{
    if( state_ != EnvState::Preparing )
        return;   // 그 사이 취소되었거나 상태가 바뀌었다.

    if( !ok )
    {
        setLastError( errorMessage );
        emit bootstrapLog( errorMessage );
        emit failed( errorMessage );
        setState( EnvState::Failed );
        return;
    }

    startSyncTask();
}

QProcessEnvironment PythonEnvManager::uvEnvironment() const
{
    QProcessEnvironment env = utf8ProcessEnvironment();

    // 포터블 EXE 모델을 지키려면 uv 가 %LOCALAPPDATA% 가 아니라 Environment/
    // 아래에만 쓰게 해야 한다. UV_PYTHON_INSTALL_DIR 이 없으면 관리형 CPython 이
    // 사용자 프로필로 떨어져 잠긴 환경에서 실패한다.
    env.insert( QStringLiteral( "UV_PYTHON_INSTALL_DIR" ), QDir( runtimeRoot() ).filePath( QStringLiteral( "python" ) ) );
    env.insert( QStringLiteral( "UV_CACHE_DIR" ), QDir( cacheDir() ).filePath( QStringLiteral( "uv" ) ) );
    env.insert( QStringLiteral( "UV_PROJECT_ENVIRONMENT" ), venvDir() );
    env.insert( QStringLiteral( "UV_PYTHON_PREFERENCE" ), QStringLiteral( "only-managed" ) );
    env.insert( QStringLiteral( "UV_NO_CONFIG" ), QStringLiteral( "1" ) );
    return env;
}

QStringList PythonEnvManager::syncArguments() const
{
    QStringList arguments{ QStringLiteral( "sync" ), QStringLiteral( "--frozen" ) };
    if( installOptionalThemes_ )
        arguments << QStringLiteral( "--extra" ) << QStringLiteral( "themes" );
    if( installOptionalExtensions_ )
        arguments << QStringLiteral( "--extra" ) << QStringLiteral( "extensions" );
    return arguments;
}

void PythonEnvManager::startSyncTask()
{
    setState( EnvState::Syncing );
    emit progressChanged( -1, tr( "패키지 확인 중" ) );

    UvTask::Request request;
    request.program = uvExecutable();
    request.arguments = syncArguments();
    request.workingDirectory = projectDir();
    request.environment = uvEnvironment();
    request.tag = QStringLiteral( "uv sync" );

    auto* task = new UvTask( std::move( request ), this );
    activeTask_ = task;

    connect( task, &UvTask::outputLine, this, [this]( const QString& line ) {
        emit bootstrapLog( line );
        updateProgressFromUvLine( line );
    } );
    connect( task, &UvTask::failedToStart, this, [this, task]( const QString& message ) {
        setLastError( message );
        emit bootstrapLog( message );
        emit failed( message );
        setState( EnvState::Failed );
        task->deleteLater();
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        task->deleteLater();

        if( cancelled )
        {
            emit bootstrapLog( tr( "환경 구성을 취소했습니다." ) );
            // ready.marker 는 맨 마지막에 쓰므로 부분 설치는 그대로 두어도 안전하다.
            setState( EnvState::Cancelled );
            return;
        }

        if( crashed || exitCode != 0 )
        {
            const QString message = tr( "uv sync 실패 (종료 코드 %1)" ).arg( exitCode );
            setLastError( message );
            emit bootstrapLog( message );
            emit failed( message );
            setState( EnvState::Failed );
            return;
        }

        startVerifyTask();
    } );

    task->start();
}

void PythonEnvManager::startVerifyTask()
{
    setState( EnvState::Verifying );
    emit progressChanged( 95, tr( "검증 중" ) );

    if( !installedFilesPresent() )
    {
        const QString message = tr( "설치는 끝났지만 python/sphinx-build/esbonio 를 찾을 수 없습니다." );
        setLastError( message );
        emit bootstrapLog( message );
        emit failed( message );
        setState( EnvState::Failed );
        return;
    }

    UvTask::Request request;
    request.program = pythonExe();
    request.arguments = { QStringLiteral( "-c" ),
                         QStringLiteral( "import sphinx, esbonio, docutils; print(sphinx.__version__)" ) };
    request.workingDirectory = projectDir();
    request.environment = utf8ProcessEnvironment();
    request.tag = QStringLiteral( "verify" );

    auto* task = new UvTask( std::move( request ), this );
    activeTask_ = task;

    connect( task, &UvTask::outputLine, this, [this]( const QString& line ) {
        emit bootstrapLog( tr( "Sphinx %1" ).arg( line ) );
    } );
    connect( task, &UvTask::failedToStart, this, [this, task]( const QString& message ) {
        setLastError( message );
        emit failed( message );
        setState( EnvState::Failed );
        task->deleteLater();
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        task->deleteLater();

        if( cancelled )
        {
            setState( EnvState::Cancelled );
            return;
        }

        if( crashed || exitCode != 0 )
        {
            const QString message = tr( "설치된 Python 환경에서 sphinx/esbonio 를 가져올 수 없습니다." );
            setLastError( message );
            emit bootstrapLog( message );
            emit failed( message );
            setState( EnvState::Failed );
            return;
        }

        // 검증까지 통과한 뒤에야 준비 표식을 남긴다.
        writeReadyMarker();
        emit progressChanged( 100, tr( "완료" ) );
        emit bootstrapLog( tr( "환경 구성 완료: %1" ).arg( nativePath( pythonExe() ) ) );
        setState( EnvState::Ready );
    } );

    task->start();
}

void PythonEnvManager::cancel()
{
    if( activeTask_ )
        activeTask_->cancel();
}

void PythonEnvManager::updateProgressFromUvLine( const QString& line )
{
    // uv 출력에서 대략적인 단계만 읽는다. 정확한 비율은 노리지 않는다.
    if( line.startsWith( QStringLiteral( "Downloading cpython" ) ) )
        emit progressChanged( 10, tr( "Python 내려받는 중" ) );
    else if( line.startsWith( QStringLiteral( "Resolved" ) ) )
        emit progressChanged( 30, tr( "의존성 해석 완료" ) );
    else if( line.startsWith( QStringLiteral( "Prepared" ) ) )
        emit progressChanged( 70, tr( "패키지 준비 완료" ) );
    else if( line.startsWith( QStringLiteral( "Installed" ) ) )
        emit progressChanged( 90, tr( "설치 완료" ) );
}

void PythonEnvManager::requestUvVersionAsync()
{
    const QString uvPath = uvExecutable();
    if( !QFileInfo::exists( uvPath ) )
    {
        emit uvVersionReady( tr( "확인 실패" ) );
        return;
    }

    UvTask::Request request;
    request.program = uvPath;
    request.arguments = { QStringLiteral( "--version" ) };
    request.tag = QStringLiteral( "uv --version" );

    auto* task = new UvTask( std::move( request ), this );
    auto* captured = new QString;

    connect( task, &UvTask::outputLine, this, [captured]( const QString& line ) {
        if( captured->isEmpty() )
            *captured = line;
    } );
    connect( task, &UvTask::failedToStart, this, [this, task, captured]( const QString& ) {
        emit uvVersionReady( tr( "확인 실패" ) );
        delete captured;
        task->deleteLater();
    } );
    connect( task, &UvTask::finished, this, [this, task, captured]( const int exitCode, const bool crashed ) {
        emit uvVersionReady( ( crashed || exitCode != 0 || captured->isEmpty() ) ? tr( "확인 실패" ) : *captured );
        delete captured;
        task->deleteLater();
    } );

    task->start();
}

}  // namespace mrst
