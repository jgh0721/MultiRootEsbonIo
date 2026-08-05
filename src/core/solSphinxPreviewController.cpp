#include "stdafx.h"
#include "solSphinxPreviewController.hpp"

#include "solUvTaskRunner.hpp"

#include <QCryptographicHash>
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

/// 출력 디렉터리 이름의 랜덤 부분. uuid 32자를 그대로 쓰면 깊은 docname 과
/// 합쳐져 Windows MAX_PATH 에 걸린다.
QString shortToken( const QString& seed, const int serial )
{
    const QByteArray digest = QCryptographicHash::hash(
        ( seed + QString::number( serial ) ).toUtf8(), QCryptographicHash::Sha1 );
    return QString::fromLatin1( digest.toHex().left( 8 ) );
}

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
    pending_ = request;
    hasPending_ = true;
    debounceTimer_->start();
}

void SphinxPreviewController::buildNow( const PreviewBuildRequest& request )
{
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

QString SphinxPreviewController::allocateOutputDir()
{
    const QString buildRoot = toCanonicalQString( active_.project.buildPath );
    const QString previewRoot = QDir( buildRoot ).filePath( QStringLiteral( "preview-html" ) );
    QDir().mkpath( previewRoot );

    const QString name = QStringLiteral( "build-%1-%2" )
                             .arg( buildSerial_, 4, 10, QLatin1Char( '0' ) )
                             .arg( shortToken( previewRoot, buildSerial_ ) );
    ++buildSerial_;

    const QString outDir = QDir( previewRoot ).filePath( name );
    QDir().mkpath( outDir );
    return outDir;
}

void SphinxPreviewController::cleanupOldOutputDirs( const QString& keepDir ) const
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

    // 이미 도는 중이면 취소하고, 끝난 뒤 재무장한다.
    if( isBuilding() )
    {
        task_->cancel();
        return;
    }

    active_ = pending_;
    hasPending_ = false;

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
    activeOutDir_ = allocateOutputDir();
    activeReportPath_ = QDir( activeOutDir_ ).filePath( QStringLiteral( ".mrr-build.json" ) );

    // doctree 는 회전시키지 않고 프로젝트당 하나로 공유한다. 원본은 회전하는
    // out-dir 안에 두어 매 빌드마다 전체 재파싱을 했다. _static 잠금 문제는
    // 출력 디렉터리에만 해당하고, 빌드는 직렬화되므로 공유가 안전하다.
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
                  << QStringLiteral( "%1=%2" ).arg( active_.sourceFile, active_.shadowFile );
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
        finishBuild( false, false );
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        task->deleteLater();
        finishBuild( !crashed && exitCode == 0, cancelled );
    } );

    task->start();
}

void SphinxPreviewController::finishBuild( const bool processOk, const bool cancelled )
{
    PreviewBuildResult result = readReport( activeReportPath_ );
    result.projectId = QString::fromStdWString( active_.project.projectId );
    result.cancelled = cancelled;
    if( !processOk )
        result.ok = false;

    if( result.ok && !result.htmlPath.isEmpty() && QFileInfo::exists( result.htmlPath ) )
    {
        lastHtmlPath_ = result.htmlPath;
        cleanupOldOutputDirs( activeOutDir_ );
    }
    else if( !cancelled )
    {
        result.ok = false;
    }

    if( !cancelled )
    {
        // 진단보다 먼저 "무엇이 다시 읽혔는지" 를 알려야 소비자가 교체 범위를
        // 정할 수 있다.
        emit processedSourcesKnown( result.sources );
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
