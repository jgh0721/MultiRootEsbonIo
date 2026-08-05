#include "stdafx.h"
#include "solVirtualProjectMgr.hpp"

#include "utils/ProcessReaper.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace mrst {
namespace {

/// 가상 프로젝트용 최소 conf.py.
///
/// %1 = 프로젝트 표시 이름, %2 = root_doc(확장자 없는 파일명),
/// %3 = include_patterns 에 넣을 파일명
///
/// myst_parser 는 있을 때만 켠다. 없는데 extensions 에 넣으면 빌드가 통째로
/// 실패한다.
constexpr auto kConfTemplate = R"PY(# MultiRoot-reST 가 자동 생성한 파일입니다.
# conf.py 가 없는 단독 문서를 미리보기/진단하기 위한 임시 설정입니다.
project = "%1"
root_doc = "%2"
master_doc = root_doc

extensions = ["sphinx.ext.mathjax"]
try:
    import myst_parser  # noqa: F401
    extensions.append("myst_parser")
except ImportError:
    pass

source_suffix = {".rst": "restructuredtext"}
if "myst_parser" in extensions:
    source_suffix[".md"] = "markdown"

# 이 파일 하나만 읽는다. 그러지 않으면 같은 폴더의 무관한 문서까지 빌드된다.
include_patterns = ["%3"]
exclude_patterns = ["**/_build/**", "**/.git/**", "**/.venv/**"]

html_theme = "alabaster"
)PY";

QString sanitizedProjectName( const QString& baseName )
{
    return baseName.isEmpty() ? QStringLiteral( "문서" ) : baseName;
}

/// 임시 디렉터리 이름에 PID 를 넣는다. 크래시나 강제 종료로 남은 것을
/// 다음 실행 때 정리하되, 지금 돌고 있는 다른 인스턴스 것은 건드리지 않기 위함.
QString virtualDirTemplate()
{
    return QDir( QDir::tempPath() )
        .filePath( QStringLiteral( "mrst-virtual-%1-XXXXXX" )
                      .arg( QCoreApplication::applicationPid() ) );
}

/// 죽은 인스턴스가 남긴 임시 디렉터리를 지운다.
void sweepAbandonedDirs()
{
    static const QRegularExpression pattern( QStringLiteral( "^mrst-virtual-(\\d+)-" ) );

    QDir temp( QDir::tempPath() );
    const QFileInfoList entries =
        temp.entryInfoList( { QStringLiteral( "mrst-virtual-*" ) }, QDir::Dirs | QDir::NoDotAndDotDot );

    for( const QFileInfo& entry : entries )
    {
        const QRegularExpressionMatch match = pattern.match( entry.fileName() );
        if( !match.hasMatch() )
            continue;

        const qint64 ownerPid = match.captured( 1 ).toLongLong();
        if( ownerPid == QCoreApplication::applicationPid() || isProcessRunning( ownerPid ) )
            continue;

        QDir( entry.absoluteFilePath() ).removeRecursively();
    }
}

}  // namespace

VirtualProjectManager::VirtualProjectManager( QObject* parent )
    : QObject( parent )
{
    // QTemporaryDir 은 소멸자에서만 지운다. 강제 종료되면 남으므로 시작할 때
    // 죽은 인스턴스가 남긴 것들을 치운다.
    sweepAbandonedDirs();
}

VirtualProjectManager::~VirtualProjectManager()
{
    cleanup();
}

bool VirtualProjectManager::isSupported( const QString& filePath )
{
    const QString suffix = QFileInfo( filePath ).suffix().toLower();
    return suffix == QStringLiteral( "rst" ) || suffix == QStringLiteral( "rest" )
        || suffix == QStringLiteral( "md" );
}

QString VirtualProjectManager::projectIdFor( const QString& filePath )
{
    const QString canonical = QFileInfo( filePath ).absoluteFilePath().toCaseFolded();
    const QByteArray digest = QCryptographicHash::hash( canonical.toUtf8(), QCryptographicHash::Sha1 );
    return QStringLiteral( "virtual-" ) + QString::fromLatin1( digest.toHex().left( 12 ) );
}

const SphinxProject* VirtualProjectManager::findById( const QString& projectId ) const
{
    const auto it = handles_.constFind( projectId );
    return it == handles_.constEnd() ? nullptr : &it.value()->project;
}

const SphinxProject* VirtualProjectManager::projectFor( const QString& filePath )
{
    if( filePath.isEmpty() || !isSupported( filePath ) )
        return nullptr;

    const QFileInfo info( filePath );
    if( !info.exists() )
        return nullptr;

    const QString projectId = projectIdFor( filePath );
    if( const SphinxProject* existing = findById( projectId ) )
        return existing;

    auto directory = std::make_unique< QTemporaryDir >( virtualDirTemplate() );
    if( !directory->isValid() )
    {
        emit logMessage( tr( "가상 프로젝트용 임시 디렉터리를 만들 수 없습니다." ) );
        return nullptr;
    }
    // 앱이 비정상 종료해도 남지 않도록 자동 삭제를 켜 둔다.
    directory->setAutoRemove( true );

    const QString confDir = directory->path();
    const QString docName = info.completeBaseName();

    QFile conf( QDir( confDir ).filePath( QStringLiteral( "conf.py" ) ) );
    if( !conf.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
    {
        emit logMessage( tr( "가상 프로젝트 conf.py 를 쓸 수 없습니다." ) );
        return nullptr;
    }
    conf.write( QString::fromUtf8( kConfTemplate )
                    .arg( sanitizedProjectName( docName ), docName, info.fileName() )
                    .toUtf8() );
    conf.close();

    auto handle = std::make_shared< Handle >();
    handle->project.projectId = projectId.toStdWString();
    handle->project.confPath = toPath( QDir( confDir ).filePath( QStringLiteral( "conf.py" ) ) );
    // conf 는 임시 디렉터리, 소스는 원본이 있는 실제 디렉터리.
    handle->project.rootPath = toPath( info.absolutePath() );
    handle->project.sourcePath = toPath( info.absolutePath() );
    handle->project.rootDoc = docName.toStdString();
    // 빌드 산출물은 사용자 폴더가 아니라 임시 디렉터리에 둔다.
    handle->project.buildPath = toPath( QDir( confDir ).filePath( QStringLiteral( "build" ) ) );
    handle->directory = std::move( directory );

    handles_.insert( projectId, handle );
    emit logMessage( tr( "가상 프로젝트 생성: %1 (%2)" ).arg( info.fileName(), projectId ) );
    return &handle->project;
}

void VirtualProjectManager::cleanup()
{
    // QTemporaryDir 소멸자가 디렉터리를 지운다.
    handles_.clear();
}

}  // namespace mrst
