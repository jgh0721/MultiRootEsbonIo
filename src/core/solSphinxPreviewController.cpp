#include "stdafx.h"
#include "solSphinxPreviewController.hpp"

#include "solUvTaskRunner.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace mrst {
namespace {

constexpr int kDefaultDebounceMs = 350;

DiagnosticEntry diagnosticFromJson( const QJsonObject& object )
{
    DiagnosticEntry entry;
    entry.path = object.value( QStringLiteral( "path" ) ).toString();
    entry.uri = pathToFileUri( entry.path );
    entry.line = qMax( 1, object.value( QStringLiteral( "line" ) ).toInt( 1 ) );
    entry.character = 1;
    entry.endLine = entry.line;
    entry.endCharacter = 2;
    entry.message = object.value( QStringLiteral( "message" ) ).toString();
    entry.source = QStringLiteral( "sphinx-build" );

    const QString severity = object.value( QStringLiteral( "severity" ) ).toString();
    if( severity == QStringLiteral( "error" ) || severity == QStringLiteral( "critical" )
        || severity == QStringLiteral( "severe" ) )
        entry.severity = 1;
    else
        entry.severity = 2;

    return entry;
}

}  // namespace

SphinxPreviewController::SphinxPreviewController( QObject* parent )
    : QObject( parent )
    , debounceTimer_( new QTimer( this ) )
{
    debounceTimer_->setSingleShot( true );
    debounceTimer_->setInterval( kDefaultDebounceMs );
    connect( debounceTimer_, &QTimer::timeout, this, [this] {
        if( hasPending_ )
            startBuild();
    } );
}

SphinxPreviewController::~SphinxPreviewController()
{
    if( task_ )
        task_->cancel();
}

bool SphinxPreviewController::isBuilding() const
{
    return task_ != nullptr && task_->isRunning();
}

QString SphinxPreviewController::lastHtmlPath() const
{
    return lastHtmlPath_;
}

void SphinxPreviewController::setShadowDir( const QString& path )
{
    shadowDir_ = path;
}

void SphinxPreviewController::setDebounceInterval( const int milliseconds )
{
    debounceTimer_->setInterval( qMax( 0, milliseconds ) );
}

void SphinxPreviewController::requestBuild( const PreviewBuildRequest& request )
{
    usedFallbackPython_ = false;
    pending_ = request;
    hasPending_ = true;
    debounceTimer_->start();
}

void SphinxPreviewController::buildNow( const PreviewBuildRequest& request )
{
    usedFallbackPython_ = false;
    pending_ = request;
    hasPending_ = true;
    debounceTimer_->stop();
    startBuild();
}

void SphinxPreviewController::cancel()
{
    debounceTimer_->stop();
    hasPending_ = false;
    if( task_ )
        task_->cancel();
}

QString SphinxPreviewController::writeShadowCopy() const
{
    if( pending_.shadowFile.isEmpty() || shadowDir_.isEmpty() )
        return {};

    return pending_.shadowFile;   // 호출 측이 이미 파일로 만들어 넘긴다.
}

QString SphinxPreviewController::outputDir() const
{
    // 출력 디렉터리를 빌드마다 새로 만들면 Sphinx 의 HTML 빌더가 mtime 을 비교할
    // <outdir>/<docname>.html 이 없어 **매번 전 문서를 다시 쓴다**. doctree 를
    // 공유해 읽기를 증분으로 만들어도 쓰기가 전량이면 소용이 없다. Breathe 로
    // API 문서를 싣는 프로젝트에서는 그 재작성만 60초를 넘긴다(실측: HTML 36MB,
    // 그중 한 파일이 22MB). 디렉터리를 고정해야 증분 판정이 살아난다.
    const QString buildRoot = toCanonicalQString( active_.project.buildPath );
    const QString previewRoot = QDir( buildRoot ).filePath( QStringLiteral( "preview-html" ) );
    const QString outDir = QDir( previewRoot ).filePath( QStringLiteral( "html" ) );
    QDir().mkpath( outDir );
    return outDir;
}

void SphinxPreviewController::cleanupStaleOutputDirs( const QString& keepDir ) const
{
    const QString buildRoot = toCanonicalQString( active_.project.buildPath );
    QDir previewRoot( QDir( buildRoot ).filePath( QStringLiteral( "preview-html" ) ) );
    if( !previewRoot.exists() )
        return;

    const QString keepName = QFileInfo( keepDir ).fileName();
    const QFileInfoList entries = previewRoot.entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot );
    for( const QFileInfo& entry : entries )
    {
        if( entry.fileName() == keepName )
            continue;
        // QWebEngine 이 이전 HTML 을 물고 있으면 지워지지 않는다. 실패해도 무시하고
        // 다음 기회에 다시 시도한다.
        QDir( entry.absoluteFilePath() ).removeRecursively();
    }
}

void SphinxPreviewController::startBuild()
{
    if( !hasPending_ )
        return;

    // 이미 도는 중이면 **취소하지 않고** 그대로 둔다. hasPending_ 가 남아 있으므로
    // finishBuild() 가 끝난 뒤 이어서 처리한다.
    //
    // 예전에는 여기서 취소했다. 빌드가 몇 초로 끝나던 시절에는 최신 요청을 빨리
    // 반영하는 쪽이 이득이었지만, 세션 복원으로 탭 여러 개가 순차로 열리거나
    // 편집·탭 전환이 이어지면 진행 중인 빌드가 계속 리셋되어 프리뷰가 영영 뜨지
    // 않는다. 게다가 고정 출력 디렉터리에서는 중단된 빌드가 **쓰다 만 HTML** 을
    // 남긴다(회수는 빌더 쪽 sentinel 이 한다).
    if( isBuilding() )
        return;

    const bool continuingFallback = usedFallbackPython_;
    active_ = pending_;
    hasPending_ = false;
    if( !continuingFallback )
        usedFallbackPython_ = false;

    if( active_.pythonExe.isEmpty() || !QFileInfo::exists( active_.pythonExe ) )
    {
        emit logMessage( tr( "Python 런타임이 아직 준비되지 않아 프리뷰를 건너뜁니다." ) );
        return;
    }
    if( active_.builderScript.isEmpty() || !QFileInfo::exists( active_.builderScript ) )
    {
        emit logMessage( tr( "프리뷰 빌더 스크립트를 찾을 수 없습니다: %1" )
                            .arg( QDir::toNativeSeparators( active_.builderScript ) ) );
        return;
    }

    const QString buildRoot = toCanonicalQString( active_.project.buildPath );
    activeOutDir_ = outputDir();
    activeReportPath_ = QDir( activeOutDir_ ).filePath( QStringLiteral( ".mrr-build.json" ) );
    ++buildSerial_;

    // doctree 는 프로젝트당 하나로 공유한다. 원본은 회전하는 out-dir 안에 두어
    // 매 빌드마다 전체 재파싱을 했다. 빌드는 직렬화되므로 공유가 안전하다.
    const QString doctreeDir = QDir( buildRoot ).filePath( QStringLiteral( "preview/doctrees" ) );
    QDir().mkpath( doctreeDir );

    QStringList arguments{
        active_.builderScript,
        QStringLiteral( "--conf-dir" ), toCanonicalQString( active_.project.confPath.parent_path() ),
        QStringLiteral( "--source-dir" ), toCanonicalQString( active_.project.sourcePath ),
        QStringLiteral( "--out-dir" ), activeOutDir_,
        QStringLiteral( "--doctree-dir" ), doctreeDir,
        QStringLiteral( "--report" ), activeReportPath_,
        QStringLiteral( "--auto-fix-legacy-conf" ),
    };
    if( !active_.sourceFile.isEmpty() )
        arguments << QStringLiteral( "--primary" ) << active_.sourceFile;
    if( !active_.shadowFile.isEmpty() && !active_.sourceFile.isEmpty() )
    {
        arguments << QStringLiteral( "--shadow" )
                  << QStringLiteral( "%1=%2" ).arg( active_.sourceFile, active_.shadowFile )
                  << QStringLiteral( "--shadow-max-read-ms" )
                  << QString::number( active_.shadowMaxReadMs );
    }

    UvTask::Request request;
    request.program = active_.pythonExe;
    request.arguments = arguments;
    request.workingDirectory = toCanonicalQString( active_.project.rootPath );
    request.environment = utf8ProcessEnvironment();
    request.tag = QStringLiteral( "sphinx preview" );

    auto* task = new UvTask( std::move( request ), this );
    task_ = task;

    const QString projectId = QString::fromStdWString( active_.project.projectId );
    emit buildStarted( projectId );

    connect( task, &UvTask::outputLine, this, &SphinxPreviewController::logMessage );
    connect( task, &UvTask::failedToStart, this, [this, task]( const QString& message ) {
        emit logMessage( message );
        task->deleteLater();
        finishBuild( -1, true, false );
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        task->deleteLater();
        finishBuild( exitCode, crashed, cancelled );
    } );

    task->start();
}

void SphinxPreviewController::finishBuild( const int exitCode, const bool crashed, const bool cancelled )
{
    // 종료 코드 4 = 빌더가 sphinx/docutils 를 임포트하지 못했다.
    // 프로젝트 venv 가 문서용이 아니라 애플리케이션용인 경우가 흔하다
    // (저장소 루트의 .venv 에는 Sphinx 가 없는 식). 이때 그냥 실패시키면
    // 프리뷰가 영영 안 뜨므로 번들 런타임으로 한 번 물러선다.
    if( !cancelled && exitCode == 4 && !usedFallbackPython_
        && !active_.fallbackPythonExe.isEmpty()
        && active_.fallbackPythonExe != active_.pythonExe )
    {
        usedFallbackPython_ = true;
        emit logMessage( tr( "프로젝트 환경에 Sphinx 가 없어 내장 환경으로 다시 시도합니다." ) );

        pending_ = active_;
        pending_.pythonExe = active_.fallbackPythonExe;
        hasPending_ = true;
        startBuild();
        return;
    }

    const bool processOk = !crashed && exitCode == 0;
    PreviewBuildResult result = readReport( activeReportPath_ );
    result.projectId = QString::fromStdWString( active_.project.projectId );
    result.sourceFile = active_.sourceFile;
    result.serial = buildSerial_;
    result.cancelled = cancelled;
    if( !processOk )
        result.ok = false;

    if( result.ok && !result.htmlPath.isEmpty() && QFileInfo::exists( result.htmlPath ) )
    {
        lastHtmlPath_ = result.htmlPath;
        // 회전하던 시절의 build-NNNN-xxxx 디렉터리를 첫 성공 빌드에서 걷어낸다.
        cleanupStaleOutputDirs( activeOutDir_ );
    }
    else if( !cancelled )
    {
        result.ok = false;
    }

    if( !cancelled )
    {
        // 진단보다 먼저 "무엇이 다시 읽혔는지" 를 알려야 소비자가 교체 범위를
        // 정할 수 있다.
        emit processedSourcesKnown( result.processedSources );
        emit diagnosticsReady( QStringLiteral( "sphinx-build" ), result.diagnostics );

        if( !result.missingExtensions.isEmpty() || !result.missingThemes.isEmpty() )
        {
            emit missingDependenciesDetected( result.projectId, result.missingExtensions,
                                             result.missingThemes );
        }

        emit logMessage( result.ok ? tr( "프리뷰 빌드 완료" ) : tr( "프리뷰 빌드 실패" ) );
    }

    emit buildFinished( result );

    // 빌드 중 새 요청이 들어왔으면 이어서 처리한다.
    if( hasPending_ )
        debounceTimer_->start();
}

PreviewBuildResult SphinxPreviewController::readReport( const QString& reportPath ) const
{
    PreviewBuildResult result;

    QFile file( reportPath );
    if( !file.open( QIODevice::ReadOnly ) )
        return result;

    const QJsonDocument document = QJsonDocument::fromJson( file.readAll() );
    file.close();
    if( !document.isObject() )
        return result;

    const QJsonObject root = document.object();
    result.ok = root.value( QStringLiteral( "ok" ) ).toBool();
    result.htmlPath = root.value( QStringLiteral( "htmlPath" ) ).toString();
    result.primaryDocname = root.value( QStringLiteral( "primaryDocname" ) ).toString();
    result.sphinxVersion = root.value( QStringLiteral( "sphinxVersion" ) ).toString();
    result.htmlTheme = root.value( QStringLiteral( "htmlTheme" ) ).toString();
    result.traceback = root.value( QStringLiteral( "traceback" ) ).toString();

    const QJsonArray sources = root.value( QStringLiteral( "sources" ) ).toArray();
    for( const QJsonValue& value : sources )
        result.sources << value.toString();

    const QJsonArray processed = root.value( QStringLiteral( "processedSources" ) ).toArray();
    for( const QJsonValue& value : processed )
        result.processedSources << value.toString();
    // 예전 스키마의 리포트에는 processedSources 가 없다. 그때는 sources 가 곧
    // "이번에 처리한 것" 이었으므로 그대로 쓴다.
    if( result.processedSources.isEmpty() )
        result.processedSources = result.sources;

    const QJsonArray diagnostics = root.value( QStringLiteral( "diagnostics" ) ).toArray();
    for( const QJsonValue& value : diagnostics )
    {
        if( value.isObject() )
            result.diagnostics.push_back( diagnosticFromJson( value.toObject() ) );
    }

    const QJsonArray missing = root.value( QStringLiteral( "missingExtensions" ) ).toArray();
    for( const QJsonValue& value : missing )
    {
        const QJsonObject entry = value.toObject();
        const QString distribution = entry.value( QStringLiteral( "distribution" ) ).toString();
        const QString module = entry.value( QStringLiteral( "module" ) ).toString();
        if( !distribution.isEmpty() )
            result.missingExtensions << distribution;
        if( !module.isEmpty() )
            result.missingModules << module;
    }

    const QJsonArray themes = root.value( QStringLiteral( "missingThemes" ) ).toArray();
    for( const QJsonValue& value : themes )
        result.missingThemes << value.toString();

    return result;
}

}  // namespace mrst
