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
/// %3 = include_patterns 에 넣을 파일명, %4 = html_theme
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
    # 켜지 않으면 표 말고는 거의 아무것도 동작하지 않는다 — 취소선, 작업 목록,
    # $수식$, 정의 목록, `:::{note}` 컨테이너가 전부 꺼진 채로 렌더된다.
    # 단독 .md 는 내장 렌더러가 그리지만, 이 conf.py 는 워크스페이스 밖의 .rst 가
    # .md 를 include 하는 경우에 여전히 쓰인다.
    #
    # linkify 는 넣지 않는다. linkify-it-py 를 따로 요구하므로 그 패키지가 없는
    # 환경에서 빌드가 통째로 실패한다.
    myst_enable_extensions = [
        "strikethrough",
        "tasklist",
        "dollarmath",
        "deflist",
        "colon_fence",
    ]
    # strikethrough 는 HTML 출력에서만 동작한다는 경고를 낸다. 우리는 HTML 만
    # 만들므로 그 경고는 진단 표에 잡음일 뿐이다.
    suppress_warnings = ["myst.strikethrough"]

# 이 파일 하나만 읽는다. 그러지 않으면 같은 폴더의 무관한 문서까지 빌드된다.
include_patterns = ["%3"]
exclude_patterns = ["**/_build/**", "**/.git/**", "**/.venv/**"]

html_theme = "%4"
)PY";

/// ⚠ **번역 금지.** 이 값은 위 kConfTemplate 의 `project = "%1"` 자리에 그대로
///   들어가 파이썬 소스가 된다. 번역문에 따옴표나 역슬래시가 하나라도 있으면
///   conf.py 가 문법 오류가 되고 Sphinx 빌드가 통째로 실패한다. 화면에 보이는
///   문자열이 아니라 생성 코드의 일부다.
QString sanitizedProjectName( const QString& baseName )
{
    return baseName.isEmpty() ? QStringLiteral( "문서" ) : baseName;
}

/// 같은 이유로 테마 이름도 걸러 낸다. 이 값은 설정 파일에서 오고, 사용자가
/// ini 를 직접 고칠 수 있다. `alabaster"; import os` 한 줄이면 우리가 만든
/// conf.py 가 임의 코드를 실행한다 — Sphinx 는 conf.py 를 exec 한다.
QString sanitizedThemeName( const QString& theme )
{
    static const QRegularExpression allowed( QStringLiteral( "^[A-Za-z0-9._-]+$" ) );
    const QString trimmed = theme.trimmed();
    return allowed.match( trimmed ).hasMatch() ? trimmed : QStringLiteral( "alabaster" );
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
    // `.md` 는 여기서 받지 않는다. 내장 Markdown 렌더러가 그 파일을 그린다.
    //
    // 받으면 파일 하나마다 임시 디렉터리 + 합성 conf.py + Sphinx 프로세스 기동
    // (실측 1.7~2.6초) + Esbonio 서버 한 벌이 붙는데, 그것을 **편집 중 매번** 낸다 —
    // 가상 프로젝트는 입력 지문을 남기지 않아(requestPreviewBuild 가 inputsFile 을
    // 비워 넘긴다) 변경 감지 게이트가 절대 걸리지 않는다. 그리고 파이썬 환경이
    // 준비되기 전에는 프리뷰가 통째로 비어 있다. esbonio 는 myst 없이는 .md 에
    // 쓸 만한 진단을 내지 않으므로 잃는 것도 없다.
    const QString suffix = QFileInfo( filePath ).suffix().toLower();
    return suffix == QStringLiteral( "rst" ) || suffix == QStringLiteral( "rest" );
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
                    .arg( sanitizedProjectName( docName ), docName, info.fileName(),
                          sanitizedThemeName( htmlTheme_ ) )
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
    // 합성 conf.py 는 myst_parser 가 설치되어 있으면 켠다. 이 프로젝트에서 열리는
    // 문서는 .rst 뿐이지만(isSupported), 그 .rst 가 .md 를 include 할 수 있다.
    // 방금 쓴 파일을 그대로 읽어 판정한다 — 템플릿과 판정이 갈라질 여지를 두지 않는다.
    handle->project.mystMarkdown = confDeclaresMystMarkdown( handle->project.confPath );
    handle->directory = std::move( directory );

    handles_.insert( projectId, handle );
    emit logMessage( tr( "가상 프로젝트 생성: %1 (%2)" ).arg( info.fileName(), projectId ) );
    return &handle->project;
}

void VirtualProjectManager::setHtmlTheme( const QString& theme )
{
    if( htmlTheme_ == theme )
        return;

    htmlTheme_ = theme;
    // 이미 만든 conf.py 는 다시 읽히지 않는다. 버려서 다음 요청에 새로 쓰게 한다.
    cleanup();
}

void VirtualProjectManager::cleanup()
{
    // QTemporaryDir 소멸자가 디렉터리를 지운다.
    handles_.clear();
}

}  // namespace mrst
