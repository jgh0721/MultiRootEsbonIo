#include "stdafx.h"
#include "solRestWorkspaceController.hpp"

#include "solAppSettings.hpp"
#include "solEsbonioLspClient.hpp"
#include "solPreviewBridge.hpp"
#include "solPythonEnvMgr.hpp"
#include "solSphinxPreviewController.hpp"
#include "solSphinxProjectRegistry.hpp"
#include "editor/QBaseEditor.hpp"

#include "solSphinxDiagnosticsStore.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QWebEngineView>

namespace {
/// 한쪽 스크롤이 상대를 움직이고, 그 움직임이 다시 되돌아오는 것을 막는 창.
constexpr qint64 kSyncGuardMs = 250;
/// 스크롤 동기화의 기준점. 0.5 = 창의 정중앙.
/// 에디터 정중앙에 있는 줄이 프리뷰에서도 정중앙에 오게 한다.
constexpr double kAnchorRatio = 0.5;
/// 이보다 큰 HTML 은 핫스왑 이득이 없다. 그냥 다시 로드한다.
constexpr qsizetype kHotSwapMaxBytes = 4 * 1024 * 1024;

/// <head> 의 내용 서명. 스타일시트 구성이 그대로인지 판단하는 데 쓴다.
/// 빌드마다 달라지는 <title>/<base> 는 빼고 공백을 접어서 비교한다.
QString previewHeadSignature( const QString& html )
{
    static const QRegularExpression headRe(
        QStringLiteral( "<head[^>]*>(.*?)</head>" ),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption );
    static const QRegularExpression titleRe(
        QStringLiteral( "<title[^>]*>.*?</title>" ),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption );
    static const QRegularExpression baseRe(
        QStringLiteral( "<base[^>]*>" ), QRegularExpression::CaseInsensitiveOption );
    static const QRegularExpression spaceRe( QStringLiteral( "\\s+" ) );

    const QRegularExpressionMatch match = headRe.match( html );
    if( !match.hasMatch() )
        return {};

    QString head = match.captured( 1 );
    head.remove( titleRe );
    head.remove( baseRe );
    head.replace( spaceRe, QStringLiteral( " " ) );

    return QString::fromLatin1(
        QCryptographicHash::hash( head.trimmed().toUtf8(), QCryptographicHash::Sha1 ).toHex() );
}
}  // namespace

namespace mrst {

namespace {

/// 프로젝트를 못 찾았을 때 쓰는 표시용 이름.
QString unresolvedProjectLabel()
{
    return WorkspaceController::tr( "(프로젝트 없음)" );
}

}  // namespace

WorkspaceController::WorkspaceController( QObject* parent )
    : QObject( parent )
    , registry_( new ProjectRegistry( this ) )
    , previewController_( new SphinxPreviewController( this ) )
    , diagnosticsStore_( new DiagnosticsStore( this ) )
    , lspClient_( new LspClient( this ) )
{
    connect( lspClient_, &LspClient::logMessage, this, &WorkspaceController::logMessage );
    connect( lspClient_, &LspClient::diagnosticsReady, this,
            [this]( const QString& source, const QVector< DiagnosticEntry >& entries ) {
                // publishDiagnostics 는 파일 단위로 온다. 그 파일 것만 교체해야
                // 다른 파일 진단이 날아가지 않는다.
                const QString path = entries.isEmpty() ? QString{} : entries.first().path;
                if( path.isEmpty() )
                    return;
                diagnosticsStore_->replaceSourceForPath( source, path, entries );
                emit diagnosticsChanged( source, entries );
            } );
    connect( lspClient_, &LspClient::serverNotification, this,
            [this]( const QString& method, const QJsonObject& ) {
                if( method == QStringLiteral( "sphinx/clientCreated" ) )
                    setLspStatus( tr( "클라이언트 생성됨" ) );
                else if( method == QStringLiteral( "sphinx/appCreated" ) )
                {
                    setLspStatus( tr( "Sphinx 앱 생성됨" ) );
                    nudgeInitialBuild();
                }
                else if( method == QStringLiteral( "sphinx/clientErrored" ) )
                    setLspStatus( tr( "오류" ) );
            } );

    connect( previewController_, &SphinxPreviewController::logMessage, this, &WorkspaceController::logMessage );
    connect( previewController_, &SphinxPreviewController::processedSourcesKnown, this,
            [this]( const QStringList& sources ) { previewProcessedSources_ = sources; } );
    connect( previewController_, &SphinxPreviewController::diagnosticsReady, this,
            [this]( const QString& source, const QVector< DiagnosticEntry >& entries ) {
                // 통째로 교체하면 안 된다. doctree 를 공유하므로 증분 빌드에서는
                // 바뀐 문서만 다시 읽히고, 그 외 문서의 경고는 아예 출력되지
                // 않는다. 전체 교체하면 멀쩡한 진단이 사라진다.
                // 이번 빌드가 실제로 처리한 파일에 한해서만 교체한다.
                if( previewProcessedSources_.isEmpty() )
                {
                    emit diagnosticsChanged( source, entries );
                    return;
                }

                QHash< QString, QVector< DiagnosticEntry > > grouped;
                for( const DiagnosticEntry& entry : entries )
                    grouped[ QFileInfo( entry.path ).absoluteFilePath() ].push_back( entry );

                for( const QString& processed : previewProcessedSources_ )
                {
                    const QString key = QFileInfo( processed ).absoluteFilePath();
                    diagnosticsStore_->replaceSourceForPath( source, key, grouped.value( key ) );
                }
                emit diagnosticsChanged( source, entries );
            } );
    connect( diagnosticsStore_, &DiagnosticsStore::pathChanged, this,
            &WorkspaceController::refreshDiagnosticMarks );
    connect( previewController_, &SphinxPreviewController::missingDependenciesDetected, this,
            &WorkspaceController::missingDependenciesDetected );
    connect( previewController_, &SphinxPreviewController::buildFinished, this,
            &WorkspaceController::onPreviewFinished );

    connect( registry_, &ProjectRegistry::logMessage, this, &WorkspaceController::logMessage );
    connect( registry_, &ProjectRegistry::scanStarted, this, [this] {
        emit logMessage( tr( "Sphinx 프로젝트를 검색하는 중..." ) );
    } );
    connect( registry_, &ProjectRegistry::scanFinished, this, [this]( int count ) {
        logProjectList();
        emit projectsChanged( count );

        // 스캔 완료 시점에 이미 열려 있던 문서들의 프로젝트를 뒤늦게 확정한다.
        for( DocumentContext& context : documents_ )
        {
            if( context.projectId.isEmpty() )
                resolveProject( context );
        }
        if( activeView_ )
            setActiveDocument( activeView_ );
    } );

    reloadSettings();
}

WorkspaceController::~WorkspaceController() = default;

void WorkspaceController::setPreviewView( QWebEngineView* view )
{
    previewView_ = view;
    if( previewView_ == nullptr )
        return;

    previewBridge_ = new PreviewBridge( this );
    previewBridge_->attachTo( previewView_ );

    connect( previewView_, &QWebEngineView::loadFinished, this, [this]( const bool ok ) {
        previewLoadedOk_ = ok;
        if( !ok )
        {
            emit logMessage( tr( "프리뷰 HTML 로드 실패" ) );
            return;
        }
        previewUrl_ = previewView_->url();
        emit logMessage( tr( "프리뷰 표시: %1" ).arg( previewUrl_.fileName() ) );
    } );

    // 핫스왑 실패는 조용히 넘어가면 안 된다. 화면이 낡은 채로 남기 때문에
    // 곧바로 전체 리로드로 되돌린다.
    connect( previewBridge_, &PreviewBridge::hotSwapCompleted, this,
            [this]( const int token, const bool ok, const QString& message ) {
                if( token != hotSwapToken_ )
                    return;

                if( ok )
                {
                    syncPreviewFromEditor();
                    return;
                }

                emit logMessage( tr( "프리뷰 부분 교체 실패, 전체 다시 로드: %1" ).arg( message ) );
                if( !pendingFullLoadPath_.isEmpty() )
                {
                    previewUrl_ = QUrl::fromLocalFile( pendingFullLoadPath_ );
                    previewLoadedOk_ = false;
                    previewBridge_->resetReady();
                    previewView_->load( previewUrl_ );
                }
            } );

    // 페이지가 준비되면 현재 에디터 위치로 맞춘다.
    connect( previewBridge_, &PreviewBridge::bridgeReady, this, [this] {
        emit logMessage( tr( "프리뷰 동기화 준비됨" ) );
        // 페이지가 다시 로드된 직후이므로, 여기서 맞춰주면 재빌드 후에도
        // 스크롤 위치가 에디터와 어긋나지 않는다.
        syncPreviewFromEditor();
    } );

    // 프리뷰 -> 에디터
    connect( previewBridge_, &PreviewBridge::previewScrollChanged, this,
            [this]( const int sourceIndex, const double line, const double ratio ) {
                syncEditorFromPreview( sourceIndex, line, ratio );
            } );

    // 프리뷰 클릭 -> 에디터 이동 (클릭 지점 비율을 그대로 유지)
    connect( previewBridge_, &PreviewBridge::editorNavigationRequested, this,
            [this]( const int sourceIndex, const double line, const double ratio ) {
                syncEditorFromPreview( sourceIndex, line, ratio );
                const QString path = pathForSourceIndex( sourceIndex );
                if( !path.isEmpty() )
                    emit navigateRequested( path, static_cast< int >( line ), 1 );
            } );
}

int WorkspaceController::sourceIndexForPath( const QString& path ) const
{
    if( path.isEmpty() )
        return -1;

    const QString normalized = QFileInfo( path ).absoluteFilePath();
    for( int index = 0; index < previewSources_.size(); ++index )
    {
        if( QFileInfo( previewSources_.at( index ) ).absoluteFilePath().compare(
                normalized, Qt::CaseInsensitive ) == 0 )
            return index;
    }
    return -1;
}

QString WorkspaceController::pathForSourceIndex( const int sourceIndex ) const
{
    if( sourceIndex < 0 || sourceIndex >= previewSources_.size() )
        return {};
    return previewSources_.at( sourceIndex );
}

void WorkspaceController::syncPreviewFromEditor()
{
    if( previewBridge_ == nullptr || activeView_.isNull() )
        return;
    if( QDateTime::currentMSecsSinceEpoch() < suppressSyncUntilMs_ )
        return;

    DocumentContext* context = contextFor( activeView_ );
    if( context == nullptr || context->path.isEmpty() )
        return;

    const int sourceIndex = sourceIndexForPath( context->path );
    if( sourceIndex < 0 )
        return;   // 이 문서가 아직 프리뷰에 포함되지 않았다.

    // 에디터 창의 kAnchorRatio 높이에 실제로 보이는 (소수) 줄을 기준으로 삼는다.
    // 소수부는 에디터 쪽 자동 줄바꿈 안에서의 위치다.
    const double anchorLine = activeView_->fractionalLineAtViewportRatio( kAnchorRatio );

    // 프리뷰가 우리 때문에 움직인 것을 다시 우리에게 보고하지 않도록 막는다.
    previewBridge_->suppressScrollFeedback( static_cast< int >( kSyncGuardMs ) );
    previewBridge_->requestScrollToLine( sourceIndex, anchorLine, kAnchorRatio );
}

void WorkspaceController::syncEditorFromPreview( const int sourceIndex, const double line, const double ratio )
{
    if( activeView_.isNull() )
        return;

    const QString path = pathForSourceIndex( sourceIndex );
    DocumentContext* context = contextFor( activeView_ );
    if( context == nullptr )
        return;

    // 프리뷰가 가리키는 파일이 지금 편집 중인 파일이 아니면(include 된 조각 등)
    // 에디터를 흔들지 않는다. 이동은 클릭 경로에서 별도로 처리한다.
    if( !path.isEmpty() && QFileInfo( path ).absoluteFilePath().compare(
                               context->path, Qt::CaseInsensitive ) != 0 )
        return;

    // 에디터를 움직이면 viewportScrolled 가 나오고, 그게 다시 프리뷰를
    // 스크롤시킨다. 그 왕복을 여기서 끊는다.
    suppressSyncUntilMs_ = QDateTime::currentMSecsSinceEpoch() + kSyncGuardMs;
    activeView_->scrollFractionalLineToViewportRatio( line, ratio );
}

void WorkspaceController::setPythonEnvironment( PythonEnvManager* manager )
{
    pythonEnv_ = manager;
    if( pythonEnv_ != nullptr && previewController_ != nullptr )
        previewController_->setShadowDir( pythonEnv_->shadowDir() );
}

QString WorkspaceController::writeShadowCopy( QTextView* view, const QString& path ) const
{
    if( view == nullptr || pythonEnv_ == nullptr || !view->isModified() )
        return {};

    const QString directory = pythonEnv_->shadowDir();
    if( directory.isEmpty() || !QDir().mkpath( directory ) )
        return {};

    // 원본 경로 해시로 이름을 만들어 문서마다 파일 하나를 재사용한다.
    const QByteArray digest = QCryptographicHash::hash( path.toUtf8(), QCryptographicHash::Sha1 );
    const QString shadowPath = QDir( directory ).filePath(
        QStringLiteral( "%1.%2" ).arg( QString::fromLatin1( digest.toHex().left( 16 ) ),
                                      QFileInfo( path ).suffix() ) );

    QFile file( shadowPath );
    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
        return {};

    file.write( view->text().toUtf8() );
    file.close();
    return shadowPath;
}

void WorkspaceController::requestPreviewBuild( const bool immediate )
{
    if( shuttingDown_ || previewController_ == nullptr || pythonEnv_ == nullptr )
        return;

    // 런타임이 준비되기 전에는 조용히 넘어간다. 준비되면 다시 호출된다.
    if( !pythonEnv_->isReady() )
        return;

    QTextView* view = activeView_;
    DocumentContext* context = contextFor( view );
    if( view == nullptr || context == nullptr || context->path.isEmpty() )
        return;

    const SphinxProject* project = registry_->findById( context->projectId );
    if( project == nullptr )
        return;   // 가상 프로젝트는 Phase 7 에서 처리한다.

    PreviewBuildRequest request;
    request.project = *project;
    request.pythonExe = pythonEnv_->pythonExe();
    request.builderScript = pythonEnv_->previewBuilderScript();
    request.sourceFile = context->path;
    request.shadowFile = writeShadowCopy( view, context->path );

    if( immediate )
        previewController_->buildNow( request );
    else
        previewController_->requestBuild( request );
}

void WorkspaceController::onPreviewFinished( const PreviewBuildResult& result )
{
    if( result.cancelled || previewView_ == nullptr )
        return;

    if( !result.ok || result.htmlPath.isEmpty() )
        return;

    // data-mrr-src 인덱스를 실제 경로로 되돌리려면 빌더가 준 순서를 그대로 쓴다.
    previewSources_ = result.sources;

    showPreviewHtml( result.htmlPath, result.projectId + QLatin1Char( '\x1f' ) + result.primaryDocname );
}

void WorkspaceController::showPreviewHtml( const QString& htmlPath, const QString& documentKey )
{
    QFile file( htmlPath );
    if( !file.open( QIODevice::ReadOnly ) )
        return;

    const QByteArray raw = file.readAll();
    file.close();

    const QString html = QString::fromUtf8( raw );
    const QString headSignature = previewHeadSignature( html );
    const QUrl url = QUrl::fromLocalFile( htmlPath );

    // 핫스왑은 아래 조건이 전부 맞을 때만 한다. 하나라도 어긋나면 전체 리로드가
    // 맞다 — 스타일이 바뀌었는데 body 만 갈면 깨진 화면이 남는다.
    const bool sameDocument = ( documentKey == previewDocumentKey_ );
    const bool sameHead = ( !headSignature.isEmpty() && headSignature == previewHeadSignature_ );
    const bool bridgeUsable = ( previewBridge_ != nullptr && previewBridge_->isReady() );
    const bool userStayedOnPage = ( previewView_->url() == previewUrl_ );
    const bool sizeOk = ( raw.size() <= kHotSwapMaxBytes );

    previewDocumentKey_ = documentKey;
    previewHeadSignature_ = headSignature;

    if( sameDocument && sameHead && bridgeUsable && userStayedOnPage && previewLoadedOk_ && sizeOk )
    {
        emit logMessage( tr( "프리뷰 부분 교체(핫스왑)" ) );
        // 상대 경로(_static, 이미지)가 새 출력 디렉터리를 가리키게 해야 한다.
        const QString baseUrl = QUrl::fromLocalFile( QFileInfo( htmlPath ).absolutePath()
                                                     + QLatin1Char( '/' ) ).toString();
        pendingFullLoadPath_ = htmlPath;
        previewBridge_->requestHotSwap( html, baseUrl, ++hotSwapToken_ );
        return;
    }

    previewUrl_ = url;
    previewLoadedOk_ = false;
    if( previewBridge_ != nullptr )
        previewBridge_->resetReady();
    previewView_->load( url );
}

void WorkspaceController::setLspStatus( const QString& state )
{
    if( lspState_ == state )
        return;

    lspState_ = state;
    emit lspStatusChanged( activeProjectId_, state );
    emit logMessage( tr( "LSP: %1 (%2)" ).arg( state, activeProjectId_ ) );
}

void WorkspaceController::nudgeInitialBuild()
{
    if( lspClient_ == nullptr || !lspClient_->isRunning() )
        return;

    // Esbonio 는 didOpen 으로는 Sphinx 앱만 만들고 빌드는 돌리지 않는다
    // (sphinx_manager/manager.py 의 document_open vs document_save).
    // 그런데 진단은 빌드가 채우는 DB 에서만 나오므로, 파일을 연 것만으로는
    // 영원히 진단이 오지 않는다.
    //
    // 그래서 디스크에 쓰지 않고 didSave 알림만 보내 첫 빌드를 유발한다.
    // (파이썬 원본도 같은 수법을 쓴다.) 프로젝트당 문서당 한 번만 보낸다 —
    // 빌드 완료가 다시 이 경로를 타면 무한 반복이 된다.
    for( DocumentContext& context : documents_ )
    {
        if( context.projectId != activeProjectId_ || context.nudgedInitialBuild )
            continue;
        if( !context.syncedToServer || context.view.isNull() || context.path.isEmpty() )
            continue;

        context.nudgedInitialBuild = true;
        lspClient_->didSave( context.path, context.view->text() );
        emit logMessage( tr( "Esbonio 초기 빌드 요청: %1" ).arg( QFileInfo( context.path ).fileName() ) );
    }
}

void WorkspaceController::ensureLspForActiveDocument()
{
    if( shuttingDown_ || lspClient_ == nullptr || pythonEnv_ == nullptr || !pythonEnv_->isReady() )
        return;

    QTextView* view = activeView_;
    DocumentContext* context = contextFor( view );
    if( view == nullptr || context == nullptr || context->path.isEmpty() )
        return;

    const SphinxProject* project = registry_->findById( context->projectId );
    if( project == nullptr )
        return;   // 가상 프로젝트는 다음 단계에서 다룬다.

    const QString projectId = QString::fromStdWString( project->projectId );
    const bool needsRestart = ( !lspClient_->isRunning() || lspClient_->activeProjectId() != projectId );
    if( needsRestart )
    {
        // 프로젝트가 바뀌면 이전 서버의 진단은 더 이상 유효하지 않다.
        diagnosticsStore_->clearSource( QStringLiteral( "esbonio" ) );
        lspClient_->setHtmlStyleOverride( confDeclaresEmptyHtmlStyle( project->confPath ) );
        lspClient_->start( *project, pythonEnv_->pythonExe(), pythonEnv_->sphinxBuildExe() );
        setLspStatus( tr( "초기화됨" ) );

        // 서버가 새로 떴으므로 이 프로젝트의 열린 문서를 모두 다시 열어 준다.
        for( DocumentContext& other : documents_ )
        {
            other.syncedToServer = false;
            other.nudgedInitialBuild = false;   // 새 서버에는 다시 유발해야 한다
            if( other.projectId == projectId )
                syncDocumentToServer( other, true );
        }
        return;
    }

    syncDocumentToServer( *context, false );
}

void WorkspaceController::syncDocumentToServer( DocumentContext& context, const bool forceOpen )
{
    if( lspClient_ == nullptr || !lspClient_->isRunning() || context.view.isNull() || context.path.isEmpty() )
        return;

    const QString languageId = context.path.endsWith( QStringLiteral( ".md" ), Qt::CaseInsensitive )
                                   ? QStringLiteral( "markdown" )
                                   : QStringLiteral( "rst" );

    if( forceOpen || !context.syncedToServer )
    {
        lspClient_->didOpen( context.path, context.view->text(), languageId );
        context.syncedToServer = true;
        lspClient_->documentSymbols( context.path );
        return;
    }

    lspClient_->didChange( context.path, context.view->text() );
}

void WorkspaceController::refreshDiagnosticMarks( const QString& normalizedPath )
{
    if( diagnosticsStore_ == nullptr )
        return;

    for( auto it = documents_.begin(); it != documents_.end(); ++it )
    {
        DocumentContext& context = it.value();
        if( context.view.isNull() || context.path.isEmpty() )
            continue;
        if( context.path.toCaseFolded() != normalizedPath )
            continue;

        context.view->setDiagnosticMarks( diagnosticsStore_->forPath( context.path ) );
    }
}

ProjectRegistry* WorkspaceController::projectRegistry() const
{
    return registry_;
}

QString WorkspaceController::workspaceRoot() const
{
    return registry_->workspaceRoot();
}

QString WorkspaceController::activeProjectId() const
{
    return activeProjectId_;
}

DiagnosticsStore* WorkspaceController::diagnostics() const
{
    return diagnosticsStore_;
}

void WorkspaceController::setWorkspaceRoot( const QString& root )
{
    registry_->setWorkspaceRoot( root );
    if( registry_->workspaceRoot().isEmpty() )
        return;

    // 캐시가 있으면 즉시 사용 가능한 목록을 얻고, 그와 별개로 항상 재스캔한다.
    // (캐시는 첫 화면을 빠르게 만들 뿐 진실의 원천이 아니다.)
    if( registry_->loadCache() )
    {
        logProjectList();
        emit projectsChanged( static_cast< int >( registry_->projects().size() ) );
    }
    registry_->rescanAsync();
}

void WorkspaceController::rescanProjects()
{
    registry_->rescanAsync();
}

void WorkspaceController::reloadSettings()
{
    AppSettings settings;
    ScannerSettings scanner;
    scanner.buildDirName = settings.value( QStringLiteral( "esbonio/buildDirName" ),
                                          QStringLiteral( "_build/multiroot-rest" ) )
                              .toString()
                              .toStdString();
    registry_->setScannerSettings( std::move( scanner ) );
}

void WorkspaceController::shutdown()
{
    shuttingDown_ = true;
    if( previewController_ != nullptr )
        previewController_->cancel();
    // LSP 프로세스는 위젯 파괴 전에 정리해야 고아로 남지 않는다.
    if( lspClient_ != nullptr )
        lspClient_->stop();
    documents_.clear();
    activeView_ = nullptr;
    activeProjectId_.clear();
}

void WorkspaceController::attachDocument( QTextView* view )
{
    if( view == nullptr || shuttingDown_ || documents_.contains( view ) )
        return;

    DocumentContext context;
    context.view = view;
    context.path = view->currentFilePath().isEmpty() ? QString{} : QFileInfo( view->currentFilePath() ).absoluteFilePath();
    resolveProject( context );
    documents_.insert( view, context );

    // 편집 중에는 디바운스된 프리뷰 재빌드 + LSP 문서 동기화.
    connect( view, &QTextView::sigTextEdited, this, [this, view] {
        if( activeView_ != view )
            return;
        requestPreviewBuild( false );
        if( DocumentContext* context = contextFor( view ) )
            syncDocumentToServer( *context, false );
    } );

    // 에디터 -> 프리뷰 스크롤 동기화.
    connect( view, &QTextView::sigViewportScrolled, this, [this, view] {
        if( activeView_ == view )
            syncPreviewFromEditor();
    } );
}

void WorkspaceController::detachDocument( QTextView* view )
{
    if( view == nullptr )
        return;

    if( DocumentContext* context = contextFor( view ) )
    {
        if( context->syncedToServer && lspClient_ != nullptr && lspClient_->isRunning() )
            lspClient_->didClose( context->path );
    }

    documents_.remove( view );
    if( activeView_ == view )
    {
        activeView_ = nullptr;
        activeProjectId_.clear();
    }
}

void WorkspaceController::setActiveDocument( QTextView* view )
{
    if( shuttingDown_ )
        return;

    activeView_ = view;
    if( view == nullptr )
    {
        activeProjectId_.clear();
        return;
    }

    DocumentContext* context = contextFor( view );
    if( context == nullptr )
    {
        attachDocument( view );
        context = contextFor( view );
        if( context == nullptr )
            return;
    }

    // 파일이 저장/이름변경으로 바뀌었을 수 있으므로 경로를 다시 확인한다.
    const QString currentPath = view->currentFilePath().isEmpty() ? QString{} : QFileInfo( view->currentFilePath() ).absoluteFilePath();
    if( currentPath != context->path )
    {
        context->path = currentPath;
        context->projectId.clear();
        resolveProject( *context );
    }
    else if( context->projectId.isEmpty() )
    {
        resolveProject( *context );
    }

    if( context->projectId == activeProjectId_ )
        return;

    activeProjectId_ = context->projectId;
    emit activeProjectChanged( activeProjectId_, context->isVirtual );
    emit logMessage( tr( "활성 프로젝트: %1" )
                        .arg( activeProjectId_.isEmpty() ? unresolvedProjectLabel() : activeProjectId_ ) );

    requestPreviewBuild( true );
    ensureLspForActiveDocument();
}

void WorkspaceController::notifyDocumentSaved( QTextView* view )
{
    if( view == nullptr || shuttingDown_ )
        return;

    // 저장으로 경로가 확정되었을 수 있으니 프로젝트를 다시 해석한다.
    setActiveDocument( view );

    // Esbonio 는 저장 시점에 내부 Sphinx 빌드를 돌린다. 이게 있어야 진단이 갱신된다.
    if( DocumentContext* context = contextFor( view ); context != nullptr && context->syncedToServer )
    {
        if( lspClient_ != nullptr && lspClient_->isRunning() )
            lspClient_->didSave( context->path, view->text() );
    }
}

DocumentContext* WorkspaceController::contextFor( QTextView* view )
{
    const auto it = documents_.find( view );
    return it == documents_.end() ? nullptr : &it.value();
}

void WorkspaceController::resolveProject( DocumentContext& context )
{
    context.projectId.clear();
    context.isVirtual = false;
    if( context.path.isEmpty() )
        return;

    if( const SphinxProject* project = registry_->resolveForFile( context.path ) )
        context.projectId = QString::fromStdWString( project->projectId );
}

void WorkspaceController::logProjectList()
{
    const std::vector< SphinxProject >& projects = registry_->projects();
    emit logMessage( tr( "Sphinx 프로젝트 %1개 발견" ).arg( projects.size() ) );
    for( std::size_t idx = 0; idx < projects.size(); ++idx )
    {
        emit logMessage( QStringLiteral( "[%1]  %2 — %3" )
                            .arg( idx + 1 )
                            .arg( QString::fromStdWString( projects[ idx ].projectId ) )
                            .arg( toQString( projects[ idx ].rootPath ) ) );
    }
}

}  // namespace mrst
