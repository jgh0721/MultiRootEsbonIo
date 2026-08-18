#include "stdafx.h"
#include "solRestWorkspaceController.hpp"

#include "solAppSettings.hpp"
#include "solEsbonioLspClient.hpp"
#include "solEsbonioLspPool.hpp"
#include "solPreviewBridge.hpp"
#include "solGlossaryIndex.hpp"
#include "solRestCompletionCoordinator.hpp"
#include "solRestOutlineService.hpp"
#include "solPythonEnvMgr.hpp"
#include "solPythonEnvResolver.hpp"
#include "solSphinxPreviewController.hpp"
#include "solSphinxProjectRegistry.hpp"
#include "solVirtualProjectMgr.hpp"
#include "editor/QBaseEditor.hpp"
#include "utils/solPhaseTrace.hpp"

#include "solSphinxDiagnosticsStore.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineSettings>
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

/// 프로젝트 개요 트리에 올리는 문서 수 상한. 넘으면 잘라내고 로그로 알린다.
constexpr int kMaxProjectOutlineDocuments = 500;

}  // namespace

WorkspaceController::WorkspaceController( QObject* parent )
    : QObject( parent )
    , registry_( new ProjectRegistry( this ) )
    , previewController_( new SphinxPreviewController( this ) )
    , virtualProjects_( new VirtualProjectManager( this ) )
    , diagnosticsStore_( new DiagnosticsStore( this ) )
    , lspPool_( new LspServerPool( this ) )
    , completions_( new CompletionCoordinator( this ) )
    , glossary_( new GlossaryIndex( this ) )
{
    connect( virtualProjects_, &VirtualProjectManager::logMessage, this, &WorkspaceController::logMessage );

    // 용어집은 Esbonio 가 아니라 우리가 직접 훑는다. objects.inv 에는 이름만 있고
    // 정의 본문이 없어서 팝업에 보여 줄 것이 나오지 않는다.
    completions_->setGlossaryIndex( glossary_ );
    connect( glossary_, &GlossaryIndex::ready, this,
            [this]( const QString& projectId, int count ) {
                completions_->notifyGlossaryReady( projectId );
                if( count > 0 )
                    emit logMessage( tr( "용어집 %1개 [%2]" ).arg( count ).arg( projectId ) );
            } );

    // 자동완성 조율자는 LSP 풀을 모른다. 라우팅은 여기서만 한다.
    connect( completions_, &CompletionCoordinator::logMessage, this, &WorkspaceController::logMessage );
    connect( completions_, &CompletionCoordinator::lspCompletionRequested, this,
            [this]( const QString& path, int line, int column, const QString& triggerCharacter ) {
                LspClient* client = lspPool_->clientFor( activeProjectId_ );
                const int requestId = ( client != nullptr && client->isRunning() )
                                          ? client->completion( path, line, column, triggerCharacter )
                                          : 0;
                // 직접 연결이므로 emit 이 돌아오기 전에 id 가 등록된다.
                completions_->registerRequestId( requestId );
            } );
    connect( completions_, &CompletionCoordinator::vocabularyHarvested, this,
            [this]( const QStringList& directives, const QStringList& roles ) {
                // 같은 프로젝트의 열린 탭 전부에 먹인다. 렉서 캐시는 뷰마다 따로다.
                for( const DocumentContext& context : std::as_const( documents_ ) )
                {
                    if( context.projectId == activeProjectId_ && !context.view.isNull() )
                        context.view->feedRstCompletionVocabulary( directives, roles );
                }
            } );

    connect( lspPool_, &LspServerPool::completionsReady, this,
            [this]( const QString& projectId, int requestId, const QList< LspCompletionItem >& items ) {
                completions_->applyLspItems( projectId, requestId, items );
            } );

    // 개요: 편집 중에는 매 글자 다시 만들지 않는다.
    outlineDebounce_ = new QTimer( this );
    outlineDebounce_->setSingleShot( true );
    outlineDebounce_->setInterval( 650 );
    connect( outlineDebounce_, &QTimer::timeout, this, &WorkspaceController::refreshDocumentOutline );

    // 왕복 방지 가드에 걸려 버려진 에디터->프리뷰 동기화를 되살린다.
    // 가드가 풀린 뒤 한 번만 다시 보낸다.
    previewSyncRetry_ = new QTimer( this );
    previewSyncRetry_->setSingleShot( true );
    connect( previewSyncRetry_, &QTimer::timeout, this, &WorkspaceController::syncPreviewFromEditor );

    connect( lspPool_, &LspServerPool::documentSymbolsReady, this,
            [this]( const QString& projectId, const QString& path,
                    const QList< LspDocumentSymbol >& symbols ) {
                // LSP 응답은 활성 문서에만 반영한다. 프로젝트 개요는 문서 수가
                // 수백 개일 수 있어 파일마다 왕복하지 않고 정규식 폴백으로 채운다.
                if( symbols.isEmpty() || projectId != activeProjectId_ || activeView_.isNull() )
                    return;

                const DocumentContext* context = contextFor( activeView_ );
                if( context == nullptr )
                    return;
                if( QFileInfo( path ).absoluteFilePath() != context->path )
                    return;

                emit documentOutlineReady( context->path, toOutlineSymbols( symbols, context->path ) );
            } );

    connect( lspPool_, &LspServerPool::logMessage, this,
            [this]( const QString&, const QString& text ) { emit logMessage( text ); } );
    connect( lspPool_, &LspServerPool::diagnosticsReady, this,
            [this]( const QString&, const QString& source, const QString& path,
                    const QVector< DiagnosticEntry >& entries ) {
                // publishDiagnostics 는 파일 단위로 온다. 그 파일 것만 교체해야
                // 다른 파일 진단이 날아가지 않는다.
                //
                // 빈 배열도 반드시 반영해야 한다. 그게 "이 파일은 이제 깨끗하다"
                // 는 통보이고, 무시하면 이미 고친 오류가 영영 남는다.
                if( path.isEmpty() )
                    return;
                diagnosticsStore_->replaceSourceForPath( source, path, entries );
                emit diagnosticsChanged( source, entries );
            } );
    connect( lspPool_, &LspServerPool::serverNotification, this,
            [this]( const QString& projectId, const QString& method, const QJsonObject& ) {
                // 상태 표시는 지금 보고 있는 프로젝트 것만 반영한다.
                const bool isActive = ( projectId == activeProjectId_ );
                if( method == QStringLiteral( "sphinx/clientCreated" ) && isActive )
                    setLspStatus( tr( "클라이언트 생성됨" ) );
                else if( method == QStringLiteral( "sphinx/appCreated" ) )
                {
                    if( isActive )
                        setLspStatus( tr( "Sphinx 앱 생성됨" ) );
                    // 초기 빌드는 활성 여부와 무관하게 유발해야 한다.
                    // 백그라운드 프로젝트의 진단도 테이블에 모이기 때문이다.
                    nudgeInitialBuild( projectId );
                    // 빌드 전에 빈 완성 결과를 받았다면 이제 다시 물어볼 수 있다.
                    completions_->notifyBuildComplete( projectId );
                }
                else if( method == QStringLiteral( "sphinx/clientErrored" ) && isActive )
                    setLspStatus( tr( "오류" ) );
            } );
    connect( lspPool_, &LspServerPool::projectSpawned, this,
            &WorkspaceController::reopenDocumentsForProject );
    connect( lspPool_, &LspServerPool::projectEvicted, this, [this]( const QString& projectId ) {
        // 서버가 사라졌으니 그 프로젝트 문서들의 서버 상태도 없어진 것으로 본다.
        for( DocumentContext& context : documents_ )
        {
            if( context.projectId != projectId )
                continue;
            context.syncedToServer = false;
            context.nudgedInitialBuild = false;
            // esbonio 진단만 지운다. sphinx-build 것은 여전히 유효하다.
            diagnosticsStore_->replaceSourceForPath( QStringLiteral( "esbonio" ), context.path, {} );
        }
        emit logMessage( tr( "Esbonio 서버 종료(LRU): %1" ).arg( projectId ) );
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
    connect( previewController_, &SphinxPreviewController::buildStarted, this,
            [this]( const QString& ) { setPreviewStatus( tr( "프리뷰 빌드 중..." ) ); } );
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
        //
        // 가상 프로젝트로 잡혀 있던 문서도 반드시 다시 확인해야 한다. 워크스페이스를
        // 열기 전에 파일을 먼저 열었거나(핫 이그짓 복원 등) 스캔 전에 열렸다면
        // 가상 프로젝트가 붙는데, 그 뒤 실제 프로젝트가 발견돼도 예전에는
        // projectId 가 비어 있지 않다는 이유로 영영 승격되지 않았다.
        // 그러면 conf.py 의 테마/확장이 적용되지 않은 채로 계속 렌더링된다.
        for( DocumentContext& context : documents_ )
        {
            if( !context.projectId.isEmpty() && !context.isVirtual )
                continue;

            const QString previousProjectId = context.projectId;
            resolveProject( context );
            if( context.projectId == previousProjectId )
                continue;

            // 담당 서버가 바뀌었으므로 새 서버에 다시 열어야 한다.
            context.syncedToServer = false;
            context.nudgedInitialBuild = false;
            if( !previousProjectId.isEmpty() )
            {
                emit logMessage( tr( "프로젝트 재배정: %1 -> %2" )
                                    .arg( previousProjectId, context.projectId ) );
                // 더 이상 쓰이지 않는 가상 프로젝트 서버는 내린다.
                lspPool_->stopProject( previousProjectId );
            }
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
    applyPreviewWebSettings();

    // 큰 문서는 빌드가 끝난 뒤에도 WebEngine 이 읽는 데 오래 걸린다(이 저장소의
    // Breathe API 페이지는 HTML 하나가 22MB 다). 그 구간을 비워 두면 빌드가
    // 끝났는데도 화면이 안 바뀌는 것처럼 보이므로 진행률을 그대로 내보낸다.
    connect( previewView_, &QWebEngineView::loadProgress, this, [this]( const int percent ) {
        if( previewStatus_.isEmpty() )
            return;   // 사용자가 프리뷰 안에서 링크를 눌러 이동한 경우
        setPreviewStatus( tr( "프리뷰 로딩 중... %1%" ).arg( percent ) );
    } );

    connect( previewView_, &QWebEngineView::loadFinished, this, [this]( const bool ok ) {
        previewLoadedOk_ = ok;
        if( !ok )
        {
            emit logMessage( tr( "프리뷰 HTML 로드 실패" ) );
            setPreviewStatus( {} );
            return;
        }
        previewUrl_ = previewView_->url();
        traceP( "preview.load.end", previewView_->url().fileName() );
        // 초기 placeholder 는 setHtml 로 넣은 것이라 파일 URL 이 아니다.
        // fileName() 이 의미 없는 조각을 내놓으므로 로그를 남기지 않는다.
        if( previewUrl_.isLocalFile() )
            emit logMessage( tr( "프리뷰 표시: %1" ).arg( previewUrl_.fileName() ) );

        // 표시 해제의 기준은 bridgeReady 다. 다만 스크립트가 붙지 않는 페이지도
        // 있으므로(리소스 로드 실패 등) 여기서도 한 번 걷어 준다.
        setPreviewStatus( {} );
    } );

    // 핫스왑 실패는 조용히 넘어가면 안 된다. 화면이 낡은 채로 남기 때문에
    // 곧바로 전체 리로드로 되돌린다.
    connect( previewBridge_, &PreviewBridge::hotSwapCompleted, this,
            [this]( const int token, const bool ok, const QString& message ) {
                if( token != hotSwapToken_ )
                    return;

                if( ok )
                {
                    setPreviewStatus( {} );
                    syncPreviewFromEditor();
                    return;
                }

                emit logMessage( tr( "프리뷰 부분 교체 실패, 전체 다시 로드: %1" ).arg( message ) );
                if( !pendingFullLoadPath_.isEmpty() )
                {
                    setPreviewStatus( tr( "프리뷰 로딩 중..." ) );
                    previewUrl_ = pendingFullLoadUrl_;
                    previewLoadedOk_ = false;
                    previewBridge_->resetReady();
                    previewView_->load( previewUrl_ );
                }
                else
                {
                    setPreviewStatus( {} );
                }
            } );

    // 페이지가 준비되면 현재 에디터 위치로 맞춘다.
    connect( previewBridge_, &PreviewBridge::bridgeReady, this, [this] {
        emit logMessage( tr( "프리뷰 동기화 준비됨" ) );
        setPreviewStatus( {} );
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
                syncEditorFromPreview( sourceIndex, line, ratio, true );
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

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if( nowMs < suppressSyncUntilMs_ )
    {
        // 프리뷰가 방금 에디터를 움직였다. 지금 되돌려 보내면 왕복이 된다.
        //
        // 그렇다고 그냥 버리면 가드가 걸린 동안의 **마지막** 스크롤이 영영
        // 반영되지 않는다. 문서 끝까지 굴렸다가 맨 위로 돌아오는 것처럼 큰
        // 이동을 하면 프리뷰가 도중에 멈춰 첫 문단과 제목이 보이지 않는
        // 상태로 남는다. 가드가 풀린 직후 현재 위치로 한 번 더 맞춘다.
        if( !previewSyncRetry_->isActive() )
            previewSyncRetry_->start( static_cast< int >( suppressSyncUntilMs_ - nowMs ) + 16 );
        return;
    }
    previewSyncRetry_->stop();

    DocumentContext* context = contextFor( activeView_ );
    if( context == nullptr || context->path.isEmpty() )
        return;

    const int sourceIndex = sourceIndexForPath( context->path );
    if( sourceIndex < 0 )
        return;   // 이 문서가 아직 프리뷰에 포함되지 않았다.

    // 에디터 창의 kAnchorRatio 높이에 실제로 보이는 (소수) 줄을 기준으로 삼는다.
    // 소수부는 에디터 쪽 자동 줄바꿈 안에서의 위치다.
    //
    // 단 에디터가 문서 맨 위에 붙어 있으면 비율 기준을 쓰지 않는다. 첫 문단이
    // 여러 화면 줄로 접히면 창 한가운데 보이는 줄은 여전히 문서 앞쪽이라
    // 프리뷰가 그 줄을 한가운데로 올리려다 제목을 화면 밖으로 밀어낸다.
    // 맨 위는 맨 위로 맞추는 것이 사용자가 기대하는 결과다.
    const double anchorLine = activeView_->firstVisibleLine() <= 1
                                  ? 1.0
                                  : activeView_->fractionalLineAtViewportRatio( kAnchorRatio );

    // 이 순간부터 잠깐은 에디터가 주도권을 갖는다. 프리뷰가 (우리 때문에
    // 움직여서) 보고해 오는 위치로 에디터를 되돌리면 스크롤이 튄다.
    previewDrivenIgnoreUntilMs_ = QDateTime::currentMSecsSinceEpoch() + kSyncGuardMs;

    // 프리뷰가 우리 때문에 움직인 것을 다시 우리에게 보고하지 않도록 막는다.
    previewBridge_->suppressScrollFeedback( static_cast< int >( kSyncGuardMs ) );
    previewBridge_->requestScrollToLine( sourceIndex, anchorLine, kAnchorRatio );
}

void WorkspaceController::syncEditorFromPreview( const int sourceIndex, const double line,
                                                 const double ratio, const bool userInitiated )
{
    if( activeView_.isNull() )
        return;

    // 에디터를 굴리는 중이라면 프리뷰의 보고는 우리가 방금 만든 결과다.
    // 그것으로 에디터를 되돌리면 스크롤이 제자리에서 튄다.
    // 프리뷰를 직접 클릭한 이동은 사용자의 뜻이므로 가드를 넘긴다.
    if( !userInitiated && QDateTime::currentMSecsSinceEpoch() < previewDrivenIgnoreUntilMs_ )
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

    if( envResolver_ == nullptr )
    {
        envResolver_ = new PythonEnvResolver( manager, this );
        connect( envResolver_, &PythonEnvResolver::logMessage, this, &WorkspaceController::logMessage );
        envResolver_->setWorkspaceRoot( registry_->workspaceRoot() );
    }
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

void WorkspaceController::requestPreviewBuild( const bool immediate, const bool forceRebuild )
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

    const SphinxProject* project = lookupProject( context->projectId );
    if( project == nullptr )
        return;   // 가상 프로젝트는 Phase 7 에서 처리한다.

    // 프리뷰도 프로젝트가 정한 인터프리터로 돌린다. 그래야 그 venv 의 테마와
    // 확장이 그대로 반영된다. 빌더 스크립트는 절대 경로로 넘기므로 프로젝트
    // 환경에 아무 것도 설치하지 않는다.
    const ResolvedPythonEnv env = envResolver_->resolve( *project );

    PreviewBuildRequest request;
    request.project = *project;
    request.pythonExe = env.pythonExe.isEmpty() ? pythonEnv_->pythonExe() : env.pythonExe;
    // 프로젝트 venv 가 문서용이 아니라 애플리케이션용이면 Sphinx 가 없다.
    // 그때는 내장 환경으로 물러선다.
    request.fallbackPythonExe = pythonEnv_->pythonExe();
    request.builderScript = pythonEnv_->previewBuilderScript();
    request.sourceFile = context->path;
    request.shadowFile = previewApplyUnsavedEdits_ ? writeShadowCopy( view, context->path )
                                                   : QString();
    request.shadowMaxReadMs = previewUnsavedMaxReadMs_;
    // 가상 프로젝트(워크스페이스 밖의 단독 파일)는 지문을 남기지 않는다.
    // 빌드 산출물이 QTemporaryDir 안이라 세션과 함께 사라지므로 지문이 가리킬
    // 대상이 없고, 남기면 사용자의 .multiroot 에 임시 프로젝트 항목만 쌓인다.
    request.inputsFile = context->isVirtual
        ? QString{}
        : previewInputsFilePath( registry_->workspaceRoot(), context->projectId );

    // 실제로 요청을 보낸 문서만 기록한다. 위쪽 조기 반환들(런타임 미준비,
    // 프로젝트 미해결)에서는 기록하지 않아야 준비가 끝난 뒤 다시 시도된다.
    previewRequestedPath_ = request.sourceFile;

    // 여기서부터는 결과가 나올 때까지 시간이 걸린다. 조기 반환들을 모두 통과한
    // 뒤에만 표시를 켠다 — 프리뷰를 만들 수 없는 파일에서 켜면 영영 남는다.
    setPreviewStatus( tr( "프리뷰 준비 중..." ) );

    traceP( "preview.request",
           QStringLiteral( "%1 immediate=%2 shadow=%3" )
               .arg( QFileInfo( request.sourceFile ).fileName() )
               .arg( immediate ? 1 : 0 )
               .arg( request.shadowFile.isEmpty() ? 0 : 1 ) );

    // 입력이 하나도 안 바뀌었으면 python 을 아예 띄우지 않고 지난 산출물을 쓴다.
    // 사본(미저장 편집)이 걸린 요청과 사용자가 명시적으로 요청한 재빌드는 예외다.
    if( previewSkipUnchangedBuild_ && request.shadowFile.isEmpty() && !forceRebuild )
    {
        tryServeFromLastBuild( request, immediate );
        return;
    }

    if( immediate )
        previewController_->buildNow( request );
    else
        previewController_->requestBuild( request );
}

void WorkspaceController::tryServeFromLastBuild( const PreviewBuildRequest& request,
                                                 const bool immediate )
{
    const QString inputsFile = request.inputsFile;

    // 판정은 문서 수십 개 + breathe 라면 doxygen XML 수백 개를 stat 한다.
    // GUI 스레드에서 하면 그 자체가 멈춤이 된다.
    QPointer< WorkspaceController > guard( this );
    const quint64 generation = ++previewGateGeneration_;
    QThreadPool::globalInstance()->start( [guard, inputsFile, request, immediate, generation] {
        const bool changed = previewInputsChanged( inputsFile );

        QMetaObject::invokeMethod( guard, [guard, request, immediate, changed, generation] {
            if( guard.isNull() || guard->shuttingDown_ )
                return;
            // 판정이 도는 사이 사용자가 다른 문서로 옮겨 갔으면 버린다.
            if( generation != guard->previewGateGeneration_ )
                return;
            guard->onPreviewGateDecided( request, immediate, changed );
        }, Qt::QueuedConnection );
    } );
}

void WorkspaceController::onPreviewGateDecided( const PreviewBuildRequest& request,
                                                const bool immediate, const bool changed )
{
    if( changed )
    {
        if( immediate )
            previewController_->buildNow( request );
        else
            previewController_->requestBuild( request );
        return;
    }

    // 바뀐 것이 없다. 지난 빌드의 리포트를 그대로 읽어 화면에 올린다.
    const PreviewBuildResult cached =
        previewController_->cachedResultFor( request );
    if( !cached.ok )
    {
        // 산출물을 믿을 수 없으면 그냥 빌드한다 (안전한 방향).
        traceP( "preview.stale.miss", QStringLiteral( "cache-unusable" ) );
        if( immediate )
            previewController_->buildNow( request );
        else
            previewController_->requestBuild( request );
        return;
    }

    traceP( "preview.stale.hit", QFileInfo( cached.htmlPath ).fileName() );
    onPreviewFinished( cached );
}

void WorkspaceController::onPreviewFinished( const PreviewBuildResult& result )
{
    // 취소된 결과로는 화면을 건드리지 않는다. 다만 뒤이어 다른 빌드가 도는 것이
    // 아니라면 표시가 남으므로, 큐가 비었을 때만 걷는다.
    if( result.cancelled || previewView_ == nullptr )
    {
        if( previewController_ == nullptr || !previewController_->isBuilding() )
            setPreviewStatus( {} );
        return;
    }

    if( !result.ok || result.htmlPath.isEmpty() )
    {
        setPreviewStatus( {} );
        return;
    }

    // 빌더는 요청한 문서를 못 찾으면 root 문서(보통 index)의 HTML 로 물러선다.
    // .md 처럼 이 프로젝트가 원본으로 읽지 않는 파일을 열었을 때가 그렇다.
    // 그 결과로 화면을 갈아치우면 방금까지 보던 문서가 엉뚱한 문서로 바뀐다.
    // 프리뷰는 마지막으로 제대로 렌더된 문서를 그대로 유지한다.
    if( !result.sourceFile.isEmpty() && result.primaryDocname.isEmpty() )
    {
        emit logMessage( tr( "프리뷰를 만들 수 없는 파일입니다(이 프로젝트의 원본이 아님): %1" )
                            .arg( QFileInfo( result.sourceFile ).fileName() ) );
        setPreviewStatus( {} );
        return;
    }

    // data-mrr-src 인덱스를 실제 경로로 되돌리려면 빌더가 준 순서를 그대로 쓴다.
    previewSources_ = result.sources;
    previewPrimaryPath_ = result.sourceFile;

    showPreviewHtml( result.htmlPath, result.projectId + QLatin1Char( '\x1f' ) + result.primaryDocname,
                     result.serial );
}

void WorkspaceController::setPreviewStatus( const QString& text )
{
    if( previewStatus_ == text )
        return;

    previewStatus_ = text;
    emit previewStatusChanged( text, !text.isEmpty() );
}

void WorkspaceController::showPreviewHtml( const QString& htmlPath, const QString& documentKey,
                                           const int buildSerial )
{
    const QFileInfo htmlInfo( htmlPath );
    const qint64 size = htmlInfo.size();
    if( size <= 0 )
    {
        setPreviewStatus( {} );
        return;
    }

    // 이 HTML 을 이미 그대로 띄워 두었다면 다시 읽을 이유가 없다.
    //
    // 출력 디렉터리가 고정이라(outputDir() 주석) 변경 없는 문서의 재빌드는 Sphinx 가
    // HTML 을 **아예 다시 쓰지 않는다.** 그런데도 빌드 일련번호가 올라 URL 이 달라지므로
    // 지금까지는 매번 전체 리로드를 했다. 이 저장소의 Breathe API 페이지는 하나가
    // 6~22MB 라, 같은 내용을 다시 읽는 데만 수 초가 든다.
    const qint64 mtimeMs = htmlInfo.lastModified().toMSecsSinceEpoch();
    if( documentKey == previewDocumentKey_
        && size == previewShownSize_
        && mtimeMs == previewShownMTimeMs_
        && previewLoadedOk_
        && previewView_->url() == previewUrl_ )
    {
        traceP( "preview.load.skip",
               QStringLiteral( "%1 %2KB" ).arg( htmlInfo.fileName() ).arg( size / 1024 ) );
        setPreviewStatus( {} );
        return;
    }

    previewShownSize_ = size;
    previewShownMTimeMs_ = mtimeMs;

    // 출력 디렉터리가 프로젝트당 하나로 고정이라 같은 문서의 URL 은 매번 같다.
    // 그대로 다시 load() 하면 Chromium 이 이전 내용을 캐시에서 낼 수 있으므로
    // 빌드 일련번호를 쿼리에 실어 다른 URL 로 만든다. file:// 에서 쿼리는 파일
    // 탐색에 쓰이지 않고, _static 같은 상대 경로 해석에도 영향이 없다.
    QUrl url = QUrl::fromLocalFile( htmlPath );
    url.setQuery( QStringLiteral( "b=%1" ).arg( buildSerial ) );

    const auto loadFullPage = [this, &url, &htmlPath, size] {
        traceP( "preview.load.begin",
               QStringLiteral( "%1 %2KB" ).arg( QFileInfo( htmlPath ).fileName() ).arg( size / 1024 ) );
        setPreviewStatus( tr( "프리뷰 로딩 중..." ) );
        previewUrl_ = url;
        previewLoadedOk_ = false;
        if( previewBridge_ != nullptr )
            previewBridge_->resetReady();
        previewView_->load( url );
    };

    // 핫스왑은 아래 조건이 전부 맞을 때만 한다. 하나라도 어긋나면 전체 리로드가
    // 맞다 — 스타일이 바뀌었는데 body 만 갈면 깨진 화면이 남는다.
    const bool sameDocument = ( documentKey == previewDocumentKey_ );
    const bool bridgeUsable = ( previewBridge_ != nullptr && previewBridge_->isReady() );
    const bool userStayedOnPage = ( previewView_->url() == previewUrl_ );

    previewDocumentKey_ = documentKey;

    // 상한을 넘는 문서는 어차피 핫스왑하지 않으므로 내용을 읽을 이유가 없다.
    // Breathe 로 만든 API 페이지는 하나가 22MB 라, <head> 서명 계산에만 쓰고 버릴
    // QString 을 GUI 스레드에서 만드는 비용이 그대로 멈춤으로 보인다.
    if( size > kHotSwapMaxBytes )
    {
        // 서명을 모르는 상태이므로 다음 비교가 낡은 값을 믿지 않게 지운다.
        previewHeadSignature_.clear();
        loadFullPage();
        return;
    }

    QFile file( htmlPath );
    if( !file.open( QIODevice::ReadOnly ) )
    {
        setPreviewStatus( {} );
        return;
    }

    const QString html = QString::fromUtf8( file.readAll() );
    file.close();

    const QString headSignature = previewHeadSignature( html );
    const bool sameHead = ( !headSignature.isEmpty() && headSignature == previewHeadSignature_ );
    previewHeadSignature_ = headSignature;

    if( !sameDocument || !sameHead || !bridgeUsable || !userStayedOnPage || !previewLoadedOk_ )
    {
        loadFullPage();
        return;
    }

    emit logMessage( tr( "프리뷰 부분 교체(핫스왑)" ) );
    // 상대 경로(_static, 이미지)가 출력 디렉터리를 가리키게 해야 한다.
    const QString baseUrl = QUrl::fromLocalFile( QFileInfo( htmlPath ).absolutePath()
                                                 + QLatin1Char( '/' ) ).toString();
    pendingFullLoadPath_ = htmlPath;
    pendingFullLoadUrl_ = url;
    previewBridge_->requestHotSwap( html, baseUrl, ++hotSwapToken_ );
}

void WorkspaceController::applyPreviewWebSettings()
{
    if( previewView_ == nullptr || previewView_->page() == nullptr )
        return;

    // 프리뷰는 Sphinx 가 만든 HTML 을 file:// 로 읽는다. Chromium 은 file:// 문서가
    // 원격 출처를 여는 것을 기본적으로 막으므로, sphinxcontrib-mermaid 처럼 CDN 에서
    // 스크립트를 받아 그리는 확장은 요청 단계에서 차단되고 다이어그램이 원본
    // 텍스트 블록으로 남는다. 그래서 이 속성이 mermaid 렌더링의 전제조건이다.
    //
    // 다만 켜 두면 문서에 적힌 임의의 원격 주소로도 요청이 나가므로, 폐쇄망이나
    // 외부 요청을 원하지 않는 환경을 위해 설정으로 끌 수 있게 둔다.
    const bool allowRemote =
        AppSettings().value( QStringLiteral( "preview/allowRemoteContent" ), true ).toBool();
    const int desired = allowRemote ? 1 : 0;
    if( previewAllowRemote_ == desired )
        return;

    const bool firstApply = ( previewAllowRemote_ < 0 );
    previewAllowRemote_   = desired;

    previewView_->page()->settings()->setAttribute(
        QWebEngineSettings::LocalContentCanAccessRemoteUrls, allowRemote );

    emit logMessage( allowRemote
                        ? tr( "프리뷰: 원격 리소스 허용 (mermaid 등 CDN 렌더링)" )
                        : tr( "프리뷰: 원격 리소스 차단" ) );

    // 이미 떠 있는 페이지의 스크립트는 바뀌기 전 설정으로 한 번 실패한 뒤다.
    // 속성만 갈아도 다시 실행되지 않으므로 같은 문서를 다시 읽는다.
    if( !firstApply && previewUrl_.isLocalFile() )
    {
        setPreviewStatus( tr( "프리뷰 로딩 중..." ) );
        previewLoadedOk_ = false;
        if( previewBridge_ != nullptr )
            previewBridge_->resetReady();
        previewView_->load( previewUrl_ );
    }
}

void WorkspaceController::setLspStatus( const QString& state )
{
    if( lspState_ == state )
        return;

    lspState_ = state;
    emit lspStatusChanged( activeProjectId_, state );
    emit logMessage( tr( "LSP: %1 (%2)" ).arg( state, activeProjectId_ ) );
}

void WorkspaceController::nudgeInitialBuild( const QString& projectId )
{
    LspClient* client = lspPool_->clientFor( projectId );
    if( client == nullptr || !client->isRunning() )
        return;

    // Esbonio 는 didOpen 으로는 Sphinx 앱만 만들고 빌드는 돌리지 않는다
    // (sphinx_manager/manager.py 의 document_open vs document_save).
    // 그런데 진단은 빌드가 채우는 DB 에서만 나오므로, 파일을 연 것만으로는
    // 영원히 진단이 오지 않는다.
    //
    // 그래서 디스크에 쓰지 않고 didSave 알림만 보내 첫 빌드를 유발한다.
    // (파이썬 원본도 같은 수법을 쓴다.) 문서당 한 번만 보낸다 — 빌드 완료가
    // 다시 이 경로를 타면 무한 반복이 된다.
    for( DocumentContext& context : documents_ )
    {
        if( context.projectId != projectId || context.nudgedInitialBuild )
            continue;
        if( !context.syncedToServer || context.view.isNull() || context.path.isEmpty() )
            continue;

        context.nudgedInitialBuild = true;
        client->didSave( context.path, context.view->text() );
        emit logMessage( tr( "Esbonio 초기 빌드 요청: %1 (%2)" )
                            .arg( QFileInfo( context.path ).fileName(), projectId ) );
    }
}

void WorkspaceController::reopenDocumentsForProject( const QString& projectId )
{
    LspClient* client = lspPool_->clientFor( projectId );
    if( client == nullptr )
        return;

    for( DocumentContext& context : documents_ )
    {
        if( context.projectId != projectId )
            continue;
        context.syncedToServer = false;
        context.nudgedInitialBuild = false;
        syncDocumentToServer( context, true );
    }
}

void WorkspaceController::ensureLspForActiveDocument()
{
    if( shuttingDown_ || lspPool_ == nullptr || pythonEnv_ == nullptr || !pythonEnv_->isReady() )
        return;

    QTextView* view = activeView_;
    DocumentContext* context = contextFor( view );
    if( view == nullptr || context == nullptr || context->path.isEmpty() )
        return;

    const SphinxProject* project = lookupProject( context->projectId );
    if( project == nullptr )
        return;   // 가상 프로젝트는 다음 단계에서 다룬다.

    const QString projectId = QString::fromStdWString( project->projectId );

    // 서버 본체는 항상 번들에서, sphinx_agent 는 프로젝트가 정한 인터프리터로.
    lspPool_->setPythonPaths( pythonEnv_->pythonExe(), pythonEnv_->sphinxBuildExe() );
    const ResolvedPythonEnv env = envResolver_->resolve( *project );
    lspPool_->setSphinxPythonCommand( env.isBundled() ? QString{} : env.pythonExe );
    // activate() 보다 먼저 pin 해야, 자리가 부족할 때 지금 전환 중인 프로젝트를
    // 밀어내는 일이 없다.
    lspPool_->setPinnedProject( projectId );

    const bool wasRunning = ( lspPool_->clientFor( projectId ) != nullptr );
    LspClient* client = lspPool_->activate( *project );
    if( client == nullptr )
        return;

    if( !wasRunning )
    {
        setLspStatus( tr( "초기화됨" ) );
        return;   // projectSpawned 가 문서 재열기를 처리한다.
    }

    syncDocumentToServer( *context, false );
}

void WorkspaceController::syncDocumentToServer( DocumentContext& context, const bool forceOpen )
{
    if( context.view.isNull() || context.path.isEmpty() )
        return;

    LspClient* client = lspPool_->clientFor( context.projectId );
    if( client == nullptr || !client->isRunning() )
        return;

    const QString languageId = context.path.endsWith( QStringLiteral( ".md" ), Qt::CaseInsensitive )
                                   ? QStringLiteral( "markdown" )
                                   : QStringLiteral( "rst" );

    if( forceOpen || !context.syncedToServer )
    {
        client->didOpen( context.path, context.view->text(), languageId );
        context.syncedToServer = true;
        client->documentSymbols( context.path );
        return;
    }

    client->didChange( context.path, context.view->text() );
}

// ── 개요 ──────────────────────────────────────────────────

void WorkspaceController::refreshDocumentOutline()
{
    if( shuttingDown_ || activeView_.isNull() )
    {
        emit outlineCleared( tr( "열린 문서가 없습니다." ) );
        return;
    }

    DocumentContext* context = contextFor( activeView_ );
    if( context == nullptr || context->path.isEmpty() )
    {
        emit outlineCleared( tr( "열린 문서가 없습니다." ) );
        return;
    }

    // 폴백을 먼저 내보낸다. Esbonio 가 데워지기 전에도 개요가 비어 있지 않게.
    emit documentOutlineReady( context->path,
                              parseRstOutline( activeView_->text(), context->path ) );

    // LSP 가 살아 있으면 더 정확한 결과로 덮어쓴다.
    if( LspClient* client = lspPool_->clientFor( context->projectId );
        client != nullptr && client->isRunning() && context->syncedToServer )
    {
        client->didChange( context->path, activeView_->text() );
        client->documentSymbols( context->path );
    }
}

void WorkspaceController::refreshProjectOutline( const bool force )
{
    if( shuttingDown_ )
        return;
    if( !force && projectOutlineProjectId_ == activeProjectId_ )
        return;

    projectOutlineProjectId_ = activeProjectId_;
    const quint64 generation = ++outlineGeneration_;

    const SphinxProject* project = lookupProject( activeProjectId_ );
    if( project == nullptr )
    {
        emit projectOutlineReady( activeProjectId_, {}, 0 );
        return;
    }

    const QString sourceRoot = toQString( project->sourcePath );
    const QString rootDoc = QString::fromStdString( project->rootDoc );
    const QString projectId = activeProjectId_;
    QPointer< WorkspaceController > guard( this );

    // 문서 수백 개를 읽어 파싱하는 일이라 GUI 스레드에서 하면 눈에 띄게 멈춘다.
    QThreadPool::globalInstance()->start( [guard, sourceRoot, rootDoc, projectId, generation] {
        int total = 0;
        const QStringList paths = collectProjectDocuments( sourceRoot, rootDoc,
                                                          kMaxProjectOutlineDocuments, &total );
        QVector< OutlineDocumentEntry > entries = buildProjectOutline( sourceRoot, paths );
        const int truncated = qMax( 0, total - static_cast< int >( paths.size() ) );

        QMetaObject::invokeMethod(
            guard,
            [guard, entries = std::move( entries ), projectId, truncated, generation]() mutable {
                if( guard )
                    guard->applyProjectOutline( std::move( entries ), projectId, truncated, generation );
            },
            Qt::QueuedConnection );
    } );
}

void WorkspaceController::applyProjectOutline( QVector< OutlineDocumentEntry > documents,
                                               const QString& projectId, const int truncated,
                                               const quint64 generation )
{
    // 프로젝트가 바뀐 뒤 도착한 결과는 버린다.
    if( shuttingDown_ || generation != outlineGeneration_ || projectId != activeProjectId_ )
        return;

    emit projectOutlineReady( projectId, documents, truncated );
    if( truncated > 0 )
    {
        emit logMessage( tr( "프로젝트 개요: 문서가 많아 %1개만 표시합니다 (%2개 생략)." )
                            .arg( documents.size() )
                            .arg( truncated ) );
    }
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
    if( envResolver_ != nullptr )
        envResolver_->setWorkspaceRoot( registry_->workspaceRoot() );
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

    if( lspPool_ != nullptr )
    {
        // 값을 줄이면 풀이 즉시 초과분을 축출한다.
        lspPool_->setMaxProcesses(
            settings.value( QStringLiteral( "esbonio/maxLspProcesses" ), 3 ).toInt() );
    }

    previewApplyUnsavedEdits_ =
        settings.value( QStringLiteral( "preview/applyUnsavedEdits" ), true ).toBool();
    previewSkipUnchangedBuild_ =
        settings.value( QStringLiteral( "preview/skipUnchangedBuild" ), true ).toBool();
    // 0 은 "제한 없음" 이다. 빌더는 음수를 그 뜻으로 받는다.
    const int maxReadMs = settings.value( QStringLiteral( "preview/unsavedEditMaxReadMs" ), 2000 ).toInt();
    previewUnsavedMaxReadMs_ = ( maxReadMs > 0 ) ? maxReadMs : -1;

    applyPreviewWebSettings();
}

void WorkspaceController::beginShutdown()
{
    shuttingDown_ = true;
}

void WorkspaceController::endShutdown()
{
    shuttingDown_ = false;

    // 종료를 준비하면서 두 가지가 벌어졌다: 워커들이 협조적 취소로 일을 버렸고,
    // MainWindow 가 전역 풀의 대기 큐를 clear() 했다. 그래서 개요와 용어집이
    // 비어 있을 수 있다.
    //
    // 그냥 setActiveDocument() 로 돌아가면 안 된다 — refreshProjectOutline(false)
    // 와 refreshGlossary(false) 는 "같은 프로젝트면 건너뛴다" 는 메모를 갖고 있어
    // 다시 훑지 않는다. 그러면 취소한 뒤로 개요·용어집이 영영 빈 채로 남는다.
    // 강제로 한 번 다시 돌린다.
    refreshProjectOutline( true );
    refreshGlossary( true );
}

void WorkspaceController::shutdown()
{
    shuttingDown_ = true;
    if( previewController_ != nullptr )
        previewController_->cancelImmediately();
    // uv sync / 패키지 설치도 함께 끊는다. Environment/ 는 앱 설치 폴더 아래라
    // 살아남으면 업데이터가 교체에 실패한다.
    if( pythonEnv_ != nullptr )
        pythonEnv_->cancelImmediately();
    traceP( "ctl.preview-cancelled" );
    // LSP 프로세스는 위젯 파괴 전에 정리해야 고아로 남지 않는다.
    if( lspPool_ != nullptr )
        lspPool_->stopAll();
    traceP( "ctl.lsp-stopped" );
    // 서버를 먼저 내린 뒤에 임시 디렉터리를 지운다 (아직 물고 있을 수 있다).
    if( virtualProjects_ != nullptr )
        virtualProjects_->cleanup();
    traceP( "ctl.virtual-cleaned" );
    documents_.clear();
    activeView_ = nullptr;
    activeProjectId_.clear();
    // MainWindow 가 곧 WebEngine 을 정리한다. 우리가 먼저 참조를 놓아야
    // 그 사이에 프리뷰 콜백이 죽은 브리지를 건드리지 않는다.
    previewView_ = nullptr;
    previewBridge_ = nullptr;
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
        outlineDebounce_->start();
    } );

    // 에디터 -> 프리뷰 스크롤 동기화.
    connect( view, &QTextView::sigViewportScrolled, this, [this, view] {
        if( activeView_ == view )
            syncPreviewFromEditor();
    } );

    // 파일 로드는 비동기다. attachDocument()/setActiveDocument() 가 불릴 때는
    // 아직 텍스트가 비어 있어서, 그때 보낸 didOpen 은 빈 문서였고 개요도 비었다.
    // 로드가 끝난 지금 다시 맞춘다.
    connect( view, &QTextView::sigFileOpened, this, [this, view]( const QString& ) {
        DocumentContext* context = contextFor( view );
        if( context == nullptr )
            return;

        const QString loadedPath = view->currentFilePath().isEmpty()
                                       ? QString{}
                                       : QFileInfo( view->currentFilePath() ).absoluteFilePath();
        if( loadedPath != context->path )
        {
            context->path = loadedPath;
            context->projectId.clear();
            resolveProject( *context );
        }

        // 이미 열려 있으면 didChange 로 전체 텍스트를 다시 보낸다.
        // didOpen 을 두 번 보내는 것은 프로토콜 위반이다.
        syncDocumentToServer( *context, false );

        if( activeView_ != view )
            return;
        refreshDocumentOutline();
        requestPreviewBuild( true );
    } );

    completions_->attachEditor( view );

    // 본문의 `:role:`target`` 호버 -> 상세 팝업. 지금은 :term: 만 내용이 있다.
    // attachDocument 는 같은 뷰에 대해 두 번 불릴 수 있어 UniqueConnection 이
    // 필요하다. 그런데 그 플래그는 **멤버 함수 연결에서만** 동작하므로
    // (람다에 쓰면 Qt 가 fatal 로 죽는다) 슬롯을 그대로 연결한다.
    connect( view, &QTextView::sigRoleHovered, completions_,
            &CompletionCoordinator::showHoverDetail, Qt::UniqueConnection );
    connect( view, &QTextView::sigRoleHoverEnded, completions_,
            &CompletionCoordinator::hideHoverDetail, Qt::UniqueConnection );
}

void WorkspaceController::detachDocument( QTextView* view )
{
    if( view == nullptr )
        return;

    if( DocumentContext* context = contextFor( view ) )
    {
        if( context->syncedToServer )
        {
            if( LspClient* client = lspPool_->clientFor( context->projectId );
                client != nullptr && client->isRunning() )
            {
                client->didClose( context->path );
            }
        }
    }

    completions_->detachEditor( view );

    documents_.remove( view );
    if( activeView_ == view )
    {
        activeView_ = nullptr;
        activeProjectId_.clear();
    }
}

void WorkspaceController::beginBatchRestore()
{
    batchRestoring_ = true;
    batchPendingActive_ = nullptr;
}

void WorkspaceController::endBatchRestore()
{
    if( !batchRestoring_ )
        return;

    batchRestoring_ = false;
    if( !batchPendingActive_.isNull() )
        setActiveDocument( batchPendingActive_ );
    batchPendingActive_ = nullptr;
}

void WorkspaceController::setActiveDocument( QTextView* view )
{
    if( shuttingDown_ )
        return;

    if( batchRestoring_ )
    {
        // activeView_ 는 일부러 건드리지 않는다. 그래야 attachDocument() 의
        // sigFileOpened 람다에 있는 "activeView_ != view 면 반환" 가드가 살아
        // 있어, 복원 중 어느 탭의 비동기 로드가 끝나도 프리뷰 빌드가 나가지 않는다.
        batchPendingActive_ = view;
        return;
    }

    QTextView* previousView = activeView_;
    activeView_ = view;
    completions_->setActiveEditor( view );
    if( view == nullptr )
    {
        activeProjectId_.clear();
        completions_->setActiveProjectId( QString{} );
        outlineDebounce_->stop();
        emit outlineCleared( tr( "열린 문서가 없습니다." ) );
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

    // 프로젝트가 바뀌었는지와 문서가 바뀌었는지는 별개다.
    //
    // 예전에는 projectId 가 같으면 곧바로 반환했는데, 한 프로젝트 안에 문서가
    // 여러 개인 보통의 경우(docs/source 에 .rst 여러 개)에는 탭을 바꿔도
    // 프리뷰가 이전 문서에 멈춰 있게 된다.
    const bool projectChanged = ( context->projectId != activeProjectId_ );
    const bool documentChanged = ( previousView != view );

    // 프리뷰가 지금 문서를 따라오지 못한 상태. 프리뷰를 만들 수 없는 파일
    // (.md 등)을 거쳐 왔거나 그때의 빌드가 실패했다면 여기에 걸린다.
    // 뷰 포인터 비교만으로는 그 경우를 잡지 못해 프리뷰가 옛 문서에 멈춘다.
    const bool previewStale = !context->path.isEmpty()
                              && context->path.compare( previewRequestedPath_, Qt::CaseInsensitive ) != 0;

    if( projectChanged )
    {
        activeProjectId_ = context->projectId;
        completions_->setActiveProjectId( activeProjectId_ );
        emit activeProjectChanged( activeProjectId_, context->isVirtual );
        emit logMessage( tr( "활성 프로젝트: %1" )
                            .arg( activeProjectId_.isEmpty() ? unresolvedProjectLabel() : activeProjectId_ ) );
    }

    if( !projectChanged && !documentChanged && !previewStale )
        return;   // 같은 문서에 대한 중복 호출

    requestPreviewBuild( true );
    ensureLspForActiveDocument();

    outlineDebounce_->stop();
    refreshDocumentOutline();
    refreshProjectOutline( false );
    refreshGlossary( false );
}

void WorkspaceController::refreshGlossary( const bool force )
{
    if( shuttingDown_ || glossary_ == nullptr )
        return;

    glossary_->setActiveProjectId( activeProjectId_ );

    const SphinxProject* project = lookupProject( activeProjectId_ );
    if( project == nullptr )
        return;

    glossary_->refresh( activeProjectId_, toQString( project->sourcePath ),
                       QString::fromStdString( project->rootDoc ), force );
}

void WorkspaceController::requestCompletion()
{
    if( shuttingDown_ || activeView_.isNull() )
        return;

    // Ctrl+Space 는 사용자가 지금 이 순간을 기준으로 물어본 것이다.
    // didChange 는 디바운스되므로 먼저 흘려보내지 않으면 서버가 옛 텍스트를
    // 보고 "여기는 completion 컨텍스트가 아니다" 라고 정당하게 답한다.
    if( DocumentContext* context = contextFor( activeView_ ); context != nullptr )
        syncDocumentToServer( *context, false );

    completions_->requestExplicit();
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
        if( LspClient* client = lspPool_->clientFor( context->projectId );
            client != nullptr && client->isRunning() )
        {
            client->didSave( context->path, view->text() );
        }
    }

    // 저장한 문서가 용어집일 수 있다. 파일 하나 때문에 전부 다시 훑지만
    // 프로젝트 개요 스캔과 같은 규모라 체감되지 않는다.
    refreshGlossary( true );
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
    {
        context.projectId = QString::fromStdWString( project->projectId );
        return;
    }

    // 스캔이 아직 안 끝났으면 가상 프로젝트를 만들지 않는다. 잠시 뒤 실제
    // 프로젝트가 나타날 수 있는데, 그 사이에 임시 프로젝트를 만들면 같은
    // 파일에 서버가 두 번 붙는다.
    if( registry_->isScanning() )
        return;

    if( const SphinxProject* virtualProject = virtualProjects_->projectFor( context.path ) )
    {
        context.projectId = QString::fromStdWString( virtualProject->projectId );
        context.isVirtual = true;
    }
}

const SphinxProject* WorkspaceController::lookupProject( const QString& projectId ) const
{
    if( const SphinxProject* project = registry_->findById( projectId ) )
        return project;
    return virtualProjects_->findById( projectId );
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
