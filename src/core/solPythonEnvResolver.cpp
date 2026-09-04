#include "stdafx.h"
#include "solPythonEnvResolver.hpp"

#include "solAppSettings.hpp"
#include "solPythonEnvHealth.hpp"
#include "solPythonEnvMgr.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

namespace mrst {
namespace {

constexpr auto kOverrideGroup = "PythonEnv/projects";

/// venv 로 인정하는 디렉터리 이름들. 우선순위 순.
const QStringList& venvCandidateNames()
{
    static const QStringList names{ QStringLiteral( ".venv" ), QStringLiteral( "venv" ),
                                   QStringLiteral( "env" ) };
    return names;
}

QString venvPythonPath( const QString& venvDir )
{
#ifdef Q_OS_WIN
    return QDir( venvDir ).filePath( QStringLiteral( "Scripts/python.exe" ) );
#else
    return QDir( venvDir ).filePath( QStringLiteral( "bin/python" ) );
#endif
}

/// pyvenv.cfg 가 있으면 복구 대상이 될 수 있는 venv 로 본다.
/// 인터프리터 누락 여부는 pythonVenvDamageReason()에서 손상으로 판정한다.
bool looksLikeVenv( const QString& directory )
{
    return QFileInfo::exists( QDir( directory ).filePath( QStringLiteral( "pyvenv.cfg" ) ) );
}

QString venvRootForPython( const QString& pythonExe )
{
    QDir scriptsDir( QFileInfo( pythonExe ).absolutePath() );
#ifdef Q_OS_WIN
    if( scriptsDir.dirName().compare( QStringLiteral( "Scripts" ), Qt::CaseInsensitive ) != 0 )
        return {};
#else
    if( scriptsDir.dirName() != QStringLiteral( "bin" ) )
        return {};
#endif
    if( !scriptsDir.cdUp() )
        return {};

    const QString root = scriptsDir.absolutePath();
    return QFileInfo::exists( scriptsDir.filePath( QStringLiteral( "pyvenv.cfg" ) ) ) ? root : QString{};
}

ResolvedPythonEnv bundledEnvironment( PythonEnvManager* manager, const QString& key,
                                      const QString& reason )
{
    ResolvedPythonEnv resolved;
    resolved.projectKey = key;
    resolved.kind = EnvKind::Bundled;
    resolved.pythonExe = manager != nullptr ? manager->pythonExe() : QString{};
    resolved.originPath = manager != nullptr ? manager->venvDir() : QString{};
    resolved.reason = reason;
    return resolved;
}

}  // namespace

QString ResolvedPythonEnv::displayName() const
{
    switch( kind )
    {
        case EnvKind::Bundled:          return QObject::tr( "내장 환경" );
        case EnvKind::ProjectVenv:      return QFileInfo( originPath ).fileName();
        case EnvKind::ExplicitOverride: return QObject::tr( "지정한 환경" );
    }
    return {};
}

PythonEnvResolver::PythonEnvResolver( PythonEnvManager* manager, QObject* parent )
    : QObject( parent )
    , manager_( manager )
{
}

QString PythonEnvResolver::projectKeyFor( const std::filesystem::path& rootPath )
{
    const QString canonical = toCanonicalQString( rootPath ).toCaseFolded();
    const QByteArray digest = QCryptographicHash::hash( canonical.toUtf8(), QCryptographicHash::Sha1 );
    return QString::fromLatin1( digest.toHex().left( 16 ) );
}

void PythonEnvResolver::setWorkspaceRoot( const QString& root )
{
    if( workspaceRoot_ == root )
        return;

    workspaceRoot_ = root;
    invalidateAll();
}

void PythonEnvResolver::invalidate( const QString& projectKey )
{
    cache_.remove( projectKey );
    if( damagedKeys_.remove( projectKey ) )
        emit environmentDamageCleared( projectKey );
}

void PythonEnvResolver::invalidateAll()
{
    cache_.clear();
    const QSet< QString > damaged = damagedKeys_;
    damagedKeys_.clear();
    for( const QString& key : damaged )
        emit environmentDamageCleared( key );
}

void PythonEnvResolver::setOverride( const SphinxProject& project, const QString& pythonExe )
{
    const QString key = projectKeyFor( project.rootPath );

    AppSettings settings;
    settings.beginGroup( QLatin1String( kOverrideGroup ) );
    settings.beginGroup( key );
    if( pythonExe.isEmpty() )
    {
        settings.remove( QString{} );
    }
    else
    {
        settings.setValue( QStringLiteral( "rootPath" ), toCanonicalQString( project.rootPath ) );
        settings.setValue( QStringLiteral( "pythonExe" ), pythonExe );
    }
    settings.endGroup();
    settings.endGroup();

    invalidate( key );
}

QString PythonEnvResolver::findProjectVenv( const std::filesystem::path& rootPath,
                                           QString* originPath ) const
{
    // 프로젝트 루트에서 위로 올라가며 찾는다.
    //
    // 워크스페이스 경계에서 멈추지 않는다. 실제 저장소는 docs/ 아래에 conf.py 를
    // 두고 .venv 는 저장소 루트에 두는 배치가 흔한데, 사용자가 docs/source 를
    // 워크스페이스로 열면 경계에서 멈출 경우 그 venv 를 영영 못 찾는다.
    // 가장 가까운 조상의 venv 가 그 문서를 위한 것일 확률이 압도적으로 높다.
    // 대신 깊이를 제한하고, 어떤 것을 골랐는지 로그로 남긴다.
    QDir current( toCanonicalQString( rootPath ) );

    for( int depth = 0; depth < 8; ++depth )
    {
        for( const QString& name : venvCandidateNames() )
        {
            const QString candidate = current.filePath( name );
            if( looksLikeVenv( candidate ) )
            {
                if( originPath != nullptr )
                    *originPath = candidate;
                return venvPythonPath( candidate );
            }
        }

        if( !current.cdUp() )
            break;
    }
    return {};
}

ResolvedPythonEnv PythonEnvResolver::resolve( const SphinxProject& project )
{
    const QString key = projectKeyFor( project.rootPath );
    if( const auto it = cache_.constFind( key ); it != cache_.constEnd() )
        return it.value();

    ResolvedPythonEnv resolved;
    resolved.projectKey = key;
    QString damagedPath;
    QString damageReason;

    // 1) 사용자가 직접 지정한 것이 최우선.
    AppSettings settings;
    const QString overridePath =
        settings.value( QStringLiteral( "%1/%2/pythonExe" ).arg( QLatin1String( kOverrideGroup ), key ) )
            .toString();
    if( !overridePath.isEmpty() )
    {
        const QString overrideVenv = venvRootForPython( overridePath );
        damageReason = QFileInfo::exists( overridePath )
                           ? ( overrideVenv.isEmpty() ? QString{} : pythonVenvDamageReason( overrideVenv ) )
                           : tr( "지정한 Python 인터프리터를 찾을 수 없습니다: %1" )
                                 .arg( QDir::toNativeSeparators( overridePath ) );
        if( damageReason.isEmpty() )
        {
            resolved.kind = EnvKind::ExplicitOverride;
            resolved.pythonExe = overridePath;
            resolved.originPath = overridePath;
            resolved.reason = tr( "사용자가 지정한 인터프리터" );
        }
        else
        {
            damagedPath = overrideVenv;
            resolved = bundledEnvironment( manager_, key,
                                           tr( "지정한 Python 환경이 손상되어 내장 환경을 사용합니다." ) );
        }
    }
    else
    {
        // 2) 프로젝트 근처의 venv.
        QString originPath;
        const QString venvPython = findProjectVenv( project.rootPath, &originPath );
        if( !venvPython.isEmpty() )
        {
            damageReason = pythonVenvDamageReason( originPath );
            if( damageReason.isEmpty() )
            {
                resolved.kind = EnvKind::ProjectVenv;
                resolved.pythonExe = venvPython;
                resolved.originPath = originPath;
                resolved.reason = tr( "프로젝트 가상환경을 찾았습니다: %1" )
                                      .arg( QDir::toNativeSeparators( originPath ) );
            }
            else
            {
                damagedPath = originPath;
                resolved = bundledEnvironment( manager_, key,
                                               tr( "프로젝트 가상환경이 손상되어 내장 환경을 사용합니다." ) );
            }
        }
        else
        {
            // 3) 번들 런타임.
            resolved = bundledEnvironment( manager_, key,
                                           tr( "프로젝트 가상환경이 없어 내장 환경을 사용합니다." ) );
        }
    }

    cache_.insert( key, resolved );
    emit logMessage( tr( "Python 환경 [%1]: %2" )
                        .arg( QString::fromStdWString( project.projectId ), resolved.reason ) );
    if( !damageReason.isEmpty() && !damagedKeys_.contains( key ) )
    {
        damagedKeys_.insert( key );
        emit logMessage( tr( "Python 환경 손상 [%1]: %2" )
                             .arg( QString::fromStdWString( project.projectId ), damageReason ) );
        emit environmentDamaged( key, QString::fromStdWString( project.projectId ), damagedPath,
                                 overridePath.isEmpty() ? venvPythonPath( damagedPath ) : overridePath,
                                 damageReason );
    }
    return resolved;
}

void PythonEnvResolver::reportRuntimeFailure( const SphinxProject& project, const QString& pythonExe,
                                              const QString& reason )
{
    const QString key = projectKeyFor( project.rootPath );
    QString environmentPath;
    if( const auto it = cache_.constFind( key ); it != cache_.constEnd() && !it->isBundled() )
        environmentPath = QFileInfo( it->originPath ).isDir() ? it->originPath : venvRootForPython( pythonExe );
    if( environmentPath.isEmpty() )
        environmentPath = venvRootForPython( pythonExe );

    cache_.insert( key, bundledEnvironment( manager_, key,
                                           tr( "프로젝트 Python 실행에 실패하여 내장 환경을 사용합니다." ) ) );
    if( damagedKeys_.contains( key ) )
        return;

    damagedKeys_.insert( key );
    emit logMessage( tr( "Python 환경 손상 [%1]: %2" )
                         .arg( QString::fromStdWString( project.projectId ), reason ) );
    emit environmentDamaged( key, QString::fromStdWString( project.projectId ), environmentPath,
                             pythonExe, reason );
}

void PythonEnvResolver::clearDamage( const QString& projectKey )
{
    invalidate( projectKey );
}

}  // namespace mrst
