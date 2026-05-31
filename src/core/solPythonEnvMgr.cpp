#include "stdafx.h"
#include "core/solPythonEnvMgr.hpp"

#include "core/solAppSettings.hpp"

namespace mrst {
namespace {

constexpr auto SETTINGS_USE_EXTERNAL_UV = "PythonEnv/useExternalUv";
constexpr auto SETTINGS_EXTERNAL_UV_PATH = "PythonEnv/externalUvPath";

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

}  // namespace

PythonEnvManager::PythonEnvManager( QObject* parent )
    : QObject( parent )
{
    AppSettings settings;
    m_useExternalUv = settings.value( SETTINGS_USE_EXTERNAL_UV, false ).toBool();
    m_externalUvPath = settings.value( SETTINGS_EXTERNAL_UV_PATH ).toString();
}

QString PythonEnvManager::appDir() const
{
    return QCoreApplication::applicationDirPath();
}

QString PythonEnvManager::runtimeRoot() const
{
    return QDir( appDir() ).filePath( QStringLiteral( "Environment" ) );
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

bool PythonEnvManager::useExternalUv() const
{
    return m_useExternalUv;
}

QString PythonEnvManager::externalUvPath() const
{
    return m_externalUvPath;
}

void PythonEnvManager::setUseExternalUv( const bool enabled )
{
    m_useExternalUv = enabled;
}

void PythonEnvManager::setExternalUvPath( const QString& path )
{
    m_externalUvPath = QDir::fromNativeSeparators( path.trimmed() );
}

void PythonEnvManager::saveUvSettings() const
{
    AppSettings settings;
    settings.setValue( SETTINGS_USE_EXTERNAL_UV, m_useExternalUv );
    settings.setValue( SETTINGS_EXTERNAL_UV_PATH, m_externalUvPath );
}

bool PythonEnvManager::isReady() const
{
    return QFileInfo::exists( readyMarker() )
        && QFileInfo::exists( pythonExe() )
        && QFileInfo::exists( sphinxBuildExe() )
        && QFileInfo::exists( esbonioExe() );
}

QDateTime PythonEnvManager::configuredDate() const
{
    const QFileInfo markerInfo( readyMarker() );
    if( !markerInfo.exists() )
        return {};

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    const QDateTime created = markerInfo.birthTime();
    if( created.isValid() )
        return created;
#endif
    return markerInfo.lastModified();
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
    if( !m_useExternalUv && !QFileInfo::exists( uvPath ) )
        copyResourceFile( resourceUvSource(), embeddedUvTarget(), true, nullptr );

    QStringList lines;
    lines << ( m_useExternalUv ? tr( "외부 UV" ) : tr( "내장 UV" ) );
    lines << tr( "경로: %1" ).arg( nativePath( uvPath ) );

    if( !m_useExternalUv && !QFileInfo::exists( uvPath ) )
        lines << tr( "상태: 구성 시 리소스에서 추출됩니다." );
    else if( m_useExternalUv && !QFileInfo::exists( uvPath ) )
        lines << tr( "상태: 지정한 UV 파일을 찾을 수 없습니다." );
    else
        lines << tr( "버전: %1" ).arg( uvVersionText( uvPath ) );

    return lines.join( QLatin1Char( '\n' ) );
}

QString PythonEnvManager::lastError() const
{
    return m_lastError;
}

bool PythonEnvManager::ensureEnvironment( QWidget* dialogParent )
{
    if( isReady() )
        return true;

    return configureEnvironment( dialogParent );
}

bool PythonEnvManager::configureEnvironment( QWidget* dialogParent )
{
    if( QFileInfo::exists( readyMarker() ) )
        QFile::remove( readyMarker() );

    QString errorMessage;
    emit bootstrapLog( tr( "Python/Sphinx/Esbonio 환경 구성을 시작합니다." ) );
    const bool ok = prepareProjectFiles( &errorMessage ) && runUvSync( &errorMessage );
    if( ok && isReady() )
    {
        emit bootstrapLog( tr( "환경 구성 완료: %1" ).arg( nativePath( pythonExe() ) ) );
        setLastError( {} );
        return true;
    }

    if( errorMessage.isEmpty() )
        errorMessage = tr( "알 수 없는 이유로 Python/Esbonio 환경 구성에 실패했습니다." );

    setLastError( errorMessage );
    emit bootstrapLog( errorMessage );
    QMessageBox::critical( dialogParent, tr( "Python/Esbonio 환경 구성 실패" ), errorMessage );
    return false;
}

QString PythonEnvManager::resourcePyprojectSource() const
{
    return QStringLiteral( ":/python/pyproject.toml" );
}

QString PythonEnvManager::resourceUvSource() const
{
#ifdef Q_OS_WIN
    return QStringLiteral( ":/python/uv.exe" );
#else
    return QStringLiteral( ":/python/uv" );
#endif
}

QString PythonEnvManager::uvExecutable() const
{
    if( m_useExternalUv )
        return m_externalUvPath;
    return embeddedUvTarget();
}

QString PythonEnvManager::uvVersionText( const QString& uvPath ) const
{
    QProcess process;
    process.setProgram( uvPath );
    process.setArguments( { QStringLiteral( "--version" ) } );
    process.setProcessChannelMode( QProcess::MergedChannels );
    process.start();
    if( !process.waitForStarted( 3000 ) )
        return tr( "확인 실패" );
    process.waitForFinished( 5000 );

    const QString output = QString::fromUtf8( process.readAllStandardOutput() ).trimmed();
    if( process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || output.isEmpty() )
        return tr( "확인 실패" );
    return output;
}

bool PythonEnvManager::prepareProjectFiles( QString* errorMessage )
{
    QDir root( runtimeRoot() );
    if( !root.mkpath( QStringLiteral( "." ) ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "Environment 디렉터리를 만들 수 없습니다: %1" ).arg( nativePath( runtimeRoot() ) );
        return false;
    }

    if( !copyResourceFile( resourcePyprojectSource(), QDir( projectDir() ).filePath( QStringLiteral( "pyproject.toml" ) ), false, errorMessage ) )
        return false;

    QFile pythonVersion( QDir( projectDir() ).filePath( QStringLiteral( ".python-version" ) ) );
    if( !pythonVersion.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( ".python-version 파일을 쓸 수 없습니다: %1" ).arg( nativePath( pythonVersion.fileName() ) );
        return false;
    }
    pythonVersion.write( "3.12\n" );
    pythonVersion.close();

    if( m_useExternalUv )
    {
        if( m_externalUvPath.isEmpty() || !QFileInfo::exists( m_externalUvPath ) )
        {
            if( errorMessage != nullptr )
                *errorMessage = tr( "외부 UV 파일을 찾을 수 없습니다: %1" ).arg( nativePath( m_externalUvPath ) );
            return false;
        }
        return true;
    }

    return copyResourceFile( resourceUvSource(), embeddedUvTarget(), true, errorMessage );
}

bool PythonEnvManager::runUvSync( QString* errorMessage )
{
    const QString uvPath = uvExecutable();
    emit bootstrapLog( tr( "작업 디렉터리: %1" ).arg( nativePath( projectDir() ) ) );
    emit bootstrapLog( tr( "UV 실행: %1" ).arg( nativePath( uvPath ) ) );

    QProcess process;
    process.setProgram( uvPath );
    process.setArguments( {
        QStringLiteral( "sync" ),
        QStringLiteral( "--no-install-project" ),
        QStringLiteral( "--python" ),
        QStringLiteral( "3.12" )
    } );
    process.setWorkingDirectory( projectDir() );
    process.setProcessChannelMode( QProcess::MergedChannels );
    process.start();
    if( !process.waitForStarted( 15000 ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "uv를 시작할 수 없습니다: %1" ).arg( nativePath( uvPath ) );
        return false;
    }

    QString allOutput;
    while( process.state() != QProcess::NotRunning )
    {
        process.waitForReadyRead( 250 );
        const QString output = QString::fromUtf8( process.readAllStandardOutput() );
        if( !output.isEmpty() )
        {
            allOutput += output;
            emit bootstrapLog( output.trimmed() );
        }
        QCoreApplication::processEvents( QEventLoop::ExcludeUserInputEvents );
    }

    const QString tail = QString::fromUtf8( process.readAllStandardOutput() );
    if( !tail.isEmpty() )
    {
        allOutput += tail;
        emit bootstrapLog( tail.trimmed() );
    }

    if( process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "uv sync 실패(exit=%1):\n%2" ).arg( process.exitCode() ).arg( allOutput.trimmed() );
        return false;
    }

    QFile marker( readyMarker() );
    if( !marker.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "준비 완료 마커를 쓸 수 없습니다: %1" ).arg( nativePath( readyMarker() ) );
        return false;
    }

    QTextStream stream( &marker );
    stream << "ready=true\n";
    stream << "configured=" << QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) << "\n";
    stream << "uv=" << nativePath( uvPath ) << "\n";
    marker.close();
    return true;
}

bool PythonEnvManager::copyResourceFile( const QString& resourcePath, const QString& destinationPath,
                                         const bool executable, QString* errorMessage ) const
{
    QFile input( resourcePath );
    if( !input.open( QIODevice::ReadOnly ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "리소스 파일을 열 수 없습니다: %1" ).arg( resourcePath );
        return false;
    }

    const QFileInfo destinationInfo( destinationPath );
    QDir destinationDir = destinationInfo.dir();
    if( !destinationDir.mkpath( QStringLiteral( "." ) ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "대상 디렉터리를 만들 수 없습니다: %1" ).arg( nativePath( destinationDir.absolutePath() ) );
        return false;
    }

    if( QFileInfo::exists( destinationPath ) && !QFile::remove( destinationPath ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "기존 파일을 삭제할 수 없습니다: %1" ).arg( nativePath( destinationPath ) );
        return false;
    }

    QFile output( destinationPath );
    if( !output.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = tr( "대상 파일을 쓸 수 없습니다: %1" ).arg( nativePath( destinationPath ) );
        return false;
    }

    output.write( input.readAll() );
    output.close();

    if( executable )
    {
        QFile::setPermissions( destinationPath, QFile::permissions( destinationPath )
            | QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther );
    }
    return true;
}

void PythonEnvManager::setLastError( const QString& errorMessage )
{
    m_lastError = errorMessage;
}

}  // namespace mrst



