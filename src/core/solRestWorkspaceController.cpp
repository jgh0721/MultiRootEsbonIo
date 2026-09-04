#include "stdafx.h"
#include "solRestWorkspaceController.hpp"

#include "solAppSettings.hpp"
#include "solFileKinds.hpp"
#include "solEsbonioLspClient.hpp"
#include "solEsbonioLspPool.hpp"
#include "solPreviewBridge.hpp"
#include "solPreviewProgress.hpp"
#include "solThemeManager.hpp"
#include "solGlossaryIndex.hpp"
#include "solRstSubstitutionIndex.hpp"
#include "solMarkdownPreviewController.hpp"
#include "solRstPathIndex.hpp"
#include "solRestCompletionCoordinator.hpp"
#include "solRestOutlineService.hpp"
#include "solPythonEnvMgr.hpp"
#include "solPythonEnvResolver.hpp"
#include "solPythonEnvHealth.hpp"
#include "solSphinxBuilders.hpp"
#include "solSphinxPreviewController.hpp"
#include "solSphinxProjectRegistry.hpp"
#include "solUvTaskRunner.hpp"
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
    , markdownPreview_( new MarkdownPreviewController( this ) )
    , virtualProjects_( new VirtualProjectManager( this ) )
    , diagnosticsStore_( new DiagnosticsStore( this ) )
    , lspPool_( new LspServerPool( this ) )
    , completions_( new CompletionCoordinator( this ) )
    , glossary_( new GlossaryIndex( this ) )
    , substitutions_( new SubstitutionIndex( this ) )
    , pathIndex_( new PathIndex( this ) )
{
    connect( virtualProjects_, &VirtualProjectManager::logMessage, this, &WorkspaceController::logMessage );

    // 내장 Markdown 렌더러. 페이지 상태는 이 클래스가 계속 단독으로 갖고,
    // 컨트롤러는 "이 원문을 밀어 달라" 를 요청만 한다.
    connect( markdownPreview_, &MarkdownPreviewController::logMessage, this,
            &WorkspaceController::logMessage );
    connect( markdownPreview_, &MarkdownPreviewController::pushRequested, this,
            [this]( const QString& text, const QString& baseUrl, const QString& optionsJson,
                    const int token ) {
                if( previewBridge_ != nullptr )
                    previewBridge_->requestMarkdownRender( text, baseUrl, optionsJson, token );
            } );
    connect( markdownPreview_, &MarkdownPreviewController::renderFinished, this,
            [this]( const QString& path, const bool ok, const QString& ) {
                if( !ok )
                {
                    setPreviewStatus( {} );
                    return;
                }
                previewPrimaryPath_ = path;
                setPreviewStatus( {} );
                // 렌더가 끝난 뒤에야 좌표가 확정된다. 그 전에 맞추면 앵커 표가
                // 비어 있어 아무 일도 일어나지 않는다.
                syncPreviewFromEditor();
            } );

    // 테마가 바뀌면 색만 다시 보낸다. **페이지를 다시 읽지 않는다** — 세션에서
    // 받아 둔 mermaid(3.4MB)와 KaTeX 를 버리게 되고, 테마 전환은 흔한 조작이다.
    // 색은 CSS 변수로 꽂히므로 재렌더 한 번이면 즉시 반영된다.
    connect( &ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]( ThemeManager::Theme ) {
        DocumentContext* context = contextFor( activeView_ );
        if( context == nullptr || routeFor( *context ) != PreviewRoute::MarkdownJs )
            return;
        markdownPreview_->requestRender( context->path, textForPreview( *context ),
                                        /*immediate=*/true, /*force=*/true );
    } );

    // 용어집은 Esbonio 가 아니라 우리가 직접 훑는다. objects.inv 에는 이름만 있고
    // 정의 본문이 없어서 팝업에 보여 줄 것이 나오지 않는다.
    completions_->setGlossaryIndex( glossary_ );
    connect( glossary_, &GlossaryIndex::ready, this,
            [this]( const QString& projectId, int count ) {
                completions_->notifyGlossaryReady( projectId );
                if( count > 0 )
                    emit logMessage( tr( "용어집 %1개 [%2]" ).arg( count ).arg( projectId ) );
            } );

    // 치환도 우리가 직접 훑는다. Esbonio 에는 `|name|` 자리를 아는 기능이 없고,
    // 무엇보다 실사용에서 가장 많이 쓰이는 정의 자리인 conf.py 의 rst_prolog 는
    // 애초에 .rst 파일이 아니라 파이썬 소스 안에 있다.
    completions_->setSubstitutionIndex( substitutions_ );
    connect( substitutions_, &SubstitutionIndex::ready, this,
            [this]( const QString& projectId, int count ) {
                completions_->notifySubstitutionsReady( projectId );
                if( count > 0 )
                    emit logMessage( tr( "치환 %1개 [%2]" ).arg( count ).arg( projectId ) );
            } );

    // 경로 후보의 "프로젝트 전역" 절반. 뿌리는 srcdir 이 아니라 워크스페이스
    // 루트다 - 실사용 문서가 참조하는 이미지 대부분이 srcdir 밖에 있다.

    completions_->setPathIndex( pathIndex_ );
    connect( pathIndex_, &PathIndex::ready, this,
            [this]( const QString& root, qsizetype ) { completions_->notifyPathIndexReady( root ); } );
    connect( pathIndex_, &PathIndex::logMessage, this, &WorkspaceController::logMessage );

    // 자동완성 조율자는 LSP 풀을 모른다. 라우팅은 여기서만 한다.
    connect( completions_, &CompletionCoordinator::logMessage, this, &WorkspaceController::logMessage );
    connect( completions_, &CompletionCoordinator::lspCompletionRequested, this,
            [this]( const QString& path, int line, int column, const QString& triggerCharacter ) {
                // didChange 는 디바운스된다. 먼저 흘려보내지 않으면 서버가 옛
                // 텍스트를 보고 "여기는 completion 컨텍스트가 아니다" 라고
                // 정당하게 답한다. Ctrl+Space 경로에만 있던 것을 여기로 옮겨
                // 두 경로가 같은 상태를 보게 한다.
                if( DocumentContext* document = contextFor( activeView_ );
                    document != nullptr )
                {
                    syncDocumentToServer( *document, false );
                }

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
    connect( previewController_, &SphinxPreviewController::pythonEnvironmentDamaged, this,
            [this]( const QString& projectId, const QString& pythonExe, const QString& reason ) {
                const SphinxProject* project = lookupProject( projectId );
                if( project != nullptr && envResolver_ != nullptr )
                    envResolver_->reportRuntimeFailure( *project, pythonExe, reason );
            } );
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

                const mrst::PhaseSpan span( "diag.store",
                                           QStringLiteral( "docs=%1 entries=%2" )
                                               .arg( previewProcessedSources_.size() )
                                               .arg( entries.size() ) );

                QHash< QString, QVector< DiagnosticEntry > > grouped;
                for( const DiagnosticEntry& entry : entries )
                    grouped[ QFileInfo( entry.path ).absoluteFilePath() ].push_back( entry );

                // 한 번에 넣는다. 예전에는 처리 문서마다 replaceSourceForPath 를
                // 불렀고, 그 하나하나가 changed() 를 내어 진단 표를 처음부터 다시
                // 만들었다 — 문서 7개 프로젝트에서 빌드 한 번에 재구축 16회.
                diagnosticsStore_->replacePathsForSource( source, previewProcessedSources_, grouped );
                emit diagnosticsChanged( source, entries );
            } );
    connect( diagnosticsStore_, &DiagnosticsStore::pathChanged, this,
            &WorkspaceController::scheduleDiagnosticMarksRefresh );
    connect( previewController_, &SphinxPreviewController::missingDependenciesDetected, this,
            &WorkspaceController::missingDependenciesDetected );
    connect( previewController_, &SphinxPreviewController::buildStarted, this,
            [this]( const QString& ) {
                setPreviewStatus( tr( "프리뷰 빌드 중..." ),
                                  previewOverallPermille( PreviewPhase::BuildRead, 0, 0 ) );
            } );
    // 빌더가 문서를 읽고 쓰는 만큼 진행도를 내보낸다. 문구에 단계와 백분율을 함께
    // 담는 이유는, 막대만으로는 "무엇이 42% 인지" 를 알 수 없기 때문이다.
    connect( previewController_, &SphinxPreviewController::buildProgress, this,
            [this]( const QString&, const QString& phase, const int done, const int total ) {
                const PreviewPhase step    = previewPhaseFromTag( phase );
                const int          overall = previewOverallPermille( step, done, total );
                const int percent = total > 0 ? qRound( ( qBound( 0, done, total ) * 100.0 ) / total ) : 0;

                const QString text = step == PreviewPhase::BuildWrite
                                             ? tr( "프리뷰 빌드 · 쓰기 %1%" ).arg( percent )
                                             : tr( "프리뷰 빌드 · 읽기 %1%" ).arg( percent );
                setPreviewStatus( text, overall );
            } );
    connect( previewController_, &SphinxPreviewController::buildFinished, this,
            &WorkspaceController::onPreviewFinished );

    connect( registry_, &ProjectRegistry::logMessage, this, &WorkspaceController::logMessage );
    connect( registry_, &ProjectRegistry::scanStarted, this, [this] {
        emit logMessage( tr( "Sphinx 프로젝트를 검색하는 중..." ) );
    } );
    connect( registry_, &ProjectRegistry::scanFinished, this, [this]( int count ) {
        logProjectList();
        emit projectsChanged( count );

        // "다른 프로젝트와 동일" 의 답이 방금 생겼거나 바뀌었다. 아래 재배정보다
        // 먼저 해야 한다 — 순서가 뒤집히면 승격되지 못한 문서가 옛 테마의
        // 가상 프로젝트를 한 번 더 만든다.
        applyVirtualProjectTheme();

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
        setPreviewStatus( tr( "프리뷰 로딩 중... %1%" ).arg( percent ),
                          previewOverallPermille( PreviewPhase::Load, percent, 100 ) );
    } );

    connect( previewView_, &QWebEngineView::loadFinished, this, [this]( const bool ok ) {
        // 성공이든 실패든 이 로드는 끝났다. 실패도 반드시 내려야 같은 URL 을
        // 다시 시도할 수 있다.
        previewLoadInFlight_ = false;
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
        if( previewUrlIsOurs() )
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
                    setPreviewStatus( tr( "프리뷰 로딩 중..." ),
                                      previewOverallPermille( PreviewPhase::Load, 0, 100 ) );
                    previewUrl_ = pendingFullLoadUrl_;
                    previewLoadedOk_ = false;
                    previewLoadInFlight_ = true;
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
        // Markdown 셸이라면 큐에 걸린 원문이 여기서 나간다. 셸을 띄우는 것과
        // 원문을 미는 것은 별개 단계이고, 핸드셰이크가 그 사이에 있다.
        markdownPreview_->notifyBridgeReady();
        // 페이지가 다시 로드된 직후이므로, 여기서 맞춰주면 재빌드 후에도
        // 스크롤 위치가 에디터와 어긋나지 않는다.
        //
        // 중복 전송 판정을 먼저 지운다. 새 페이지는 맨 위에 있으므로 "지난번과
        // 같은 줄" 이어도 반드시 한 번은 보내야 한다.
        lastSyncedSourceIndex_ = -1;
        lastSyncedLine_ = -1.0;
        syncPreviewFromEditor();
    } );

    // 프리뷰 -> 에디터
    connect( previewBridge_, &PreviewBridge::previewScrollChanged, this,
            [this]( const int sourceIndex, const double line, const double ratio,
                    const bool userDriven ) {
                if( !userDriven )
                {
                    // 페이지가 스스로 움직였다 — 내용이 바뀐 뒤 이미지·폰트·수식이
                    // 자리를 잡는 중이다. 그것으로 에디터를 옮기면 편집하던 줄이
                    // 밀려난다(PreviewBridge::previewScrolled 의 userDriven 주석 참고).
                    traceP( "sync.editor.skip", QStringLiteral( "not-user" ) );
                    return;
                }
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

    // 내장 렌더러가 올리는 사실들. 문장은 컨트롤러가 만든다.
    connect( previewBridge_, &PreviewBridge::markdownRenderCompleted, markdownPreview_,
            &MarkdownPreviewController::onRenderCompleted );
    // 내장 렌더러 경로도 body 를 갈아 끼운다. 타이핑 중에 가장 자주 도는 길이라
    connect( previewBridge_, &PreviewBridge::markdownRendererOrigin, markdownPreview_,
            &MarkdownPreviewController::onRendererOrigin );
    connect( previewBridge_, &PreviewBridge::markdownAssetLoadFailed, markdownPreview_,
            &MarkdownPreviewController::onAssetFailed );
}

void WorkspaceController::setPreviewSources( const QStringList& sources )
{
    previewSources_ = sources;

    // 역인덱스를 여기서만 만든다. 키는 DiagnosticsStore::normalizeKey 와 같은
    // 규칙(절대 경로 + 대소문자 접기)이다 — Windows 는 대소문자를 구분하지 않으므로
    // 예전 선형 검색도 Qt::CaseInsensitive 로 비교했다.
    previewSourceIndex_.clear();
    previewSourceIndex_.reserve( sources.size() );
    for( int index = 0; index < sources.size(); ++index )
    {
        const QString key = QFileInfo( sources.at( index ) ).absoluteFilePath().toCaseFolded();
        if( key.isEmpty() )
            continue;
        // 같은 문서가 두 번 들어오면 **앞의 것**을 남긴다. 예전 선형 검색이
        // 첫 일치를 돌려주었으므로 그 동작을 지킨다.
        if( !previewSourceIndex_.contains( key ) )
            previewSourceIndex_.insert( key, index );
    }

    // 목록이 바뀌면 지난 인덱스는 다른 문서를 가리킬 수 있다. 중복 전송 판정을
    // 초기화해 다음 동기화가 반드시 나가게 한다.
    lastSyncedSourceIndex_ = -1;
    lastSyncedLine_ = -1.0;
}

int WorkspaceController::sourceIndexForPath( const QString& path ) const
{
    if( path.isEmpty() )
        return -1;

    return previewSourceIndex_.value( QFileInfo( path ).absoluteFilePath().toCaseFolded(), -1 );
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

    // 보낼 값이 지난번과 같으면 아무것도 하지 않는다. **가드도 세우지 않는다** —
    // 보내지 않았으므로 프리뷰가 우리 때문에 움직일 일이 없고, 그런데도 가드를
    // 세우면 그 250 ms 동안 사용자의 프리뷰 스크롤을 무시하게 된다.
    //
    // 편집기의 뷰포트 시그널은 문서 줄이 바뀌지 않는 조작에서도 나온다 —
    // 자동 줄넘김 토글(Alt+Z)이 그렇다. SCI_SETWRAPMODE 가
    // ContainerNeedsUpdate(HScroll) 를 세우므로 SCN_UPDATEUI 가 오고, 그것이
    // 그대로 여기까지 내려온다. 그때마다 24 MB 프리뷰 페이지의 렌더러에
    // 스크롤 요청이 꽂히고 250 ms 가드 둘과 재시도 타이머가 다시 걸린다.
    // 실측으로 Alt+Z 연타 구간의 60 ms 대 정체가 여기서 왔다.
    if( sourceIndex == lastSyncedSourceIndex_ && qFuzzyCompare( anchorLine, lastSyncedLine_ ) )
        return;

    lastSyncedSourceIndex_ = sourceIndex;
    lastSyncedLine_ = anchorLine;

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

    // 여기 닿는 것은 전부 사용자가 만든 이동이다 — 프리뷰를 굴렸거나(판정은 JS 가
    // 한다) 눌러서 옮겼거나. 페이지가 스스로 움직인 것은 부르는 쪽에서 이미 걸러 냈다.
    //
    // 굴린 것에만 왕복 가드를 적용한다. 에디터를 굴리는 중이면 프리뷰의 보고는
    // 우리가 방금 만든 스크롤의 결과일 수 있고, 그것으로 에디터를 되돌리면
    // 스크롤이 제자리에서 튄다. 눌러서 옮긴 것은 명백한 지시이므로 넘긴다.
    if( !userInitiated && QDateTime::currentMSecsSinceEpoch() < previewDrivenIgnoreUntilMs_ )
    {
        traceP( "sync.editor.skip", QStringLiteral( "editor-driving" ) );
        return;
    }

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
    // "편집하던 줄이 눈앞에서 사라졌다" 는 이 한 줄에서 온다. 걸러지지 않은 이동만
    // 여기 닿으므로, 트레이스에 이 태그가 몇 번 찍히는지가 곧 판정의 측정이다.
    // `click` 은 프리뷰를 눌러서 옮긴 것(1)과 굴려서 옮긴 것(0)을 가른다.
    traceP( "sync.editor",
           QStringLiteral( "line=%1 click=%2" ).arg( line, 0, 'f', 1 ).arg( userInitiated ? 1 : 0 ) );
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
        connect( envResolver_, &PythonEnvResolver::environmentDamaged, this,
                [this]( const QString& projectKey, const QString& projectId,
                        const QString& environmentPath, const QString&, const QString& reason ) {
                    DamagedPythonEnvironment damaged;
                    damaged.projectId = projectId;
                    damaged.environmentPath = environmentPath;
                    damaged.projectRoot = environmentPath.isEmpty()
                                                ? QString{}
                                                : QFileInfo( environmentPath ).absolutePath();
                    damaged.reason = reason;
                    damagedPythonEnvironments_.insert( projectKey, damaged );

                    if( lspPool_ != nullptr )
                        lspPool_->stopProject( projectId );
                    emit pythonEnvironmentDamaged( projectKey, projectId, environmentPath, reason );

                    QTimer::singleShot( 0, this, [this, projectId] {
                        if( !shuttingDown_ && activeProjectId_ == projectId )
                            ensureLspForActiveDocument();
                    } );
                } );
        connect( envResolver_, &PythonEnvResolver::environmentDamageCleared, this,
                [this]( const QString& projectKey ) {
                    damagedPythonEnvironments_.remove( projectKey );
                    emit pythonEnvironmentDamageCleared( projectKey );
                } );
        envResolver_->setWorkspaceRoot( registry_->workspaceRoot() );
    }

    if( pythonEnv_ != nullptr )
    {
        connect( pythonEnv_, &PythonEnvManager::projectRepairStarted, this,
                &WorkspaceController::pythonEnvironmentRepairStarted, Qt::UniqueConnection );
        connect( pythonEnv_, &PythonEnvManager::projectRepairProgress, this,
                &WorkspaceController::pythonEnvironmentRepairProgress, Qt::UniqueConnection );
        connect( pythonEnv_, &PythonEnvManager::projectRepairFinished, this,
                [this]( const QString& projectKey, const bool success, const QString& message ) {
                    QString projectId;
                    if( const auto it = damagedPythonEnvironments_.constFind( projectKey );
                        it != damagedPythonEnvironments_.constEnd() )
                    {
                        projectId = it->projectId;
                    }

                    if( success && envResolver_ != nullptr )
                    {
                        envResolver_->clearDamage( projectKey );
                        if( lspPool_ != nullptr && !projectId.isEmpty() )
                            lspPool_->stopProject( projectId );
                        QTimer::singleShot( 0, this, [this, projectId] {
                            if( shuttingDown_ || activeProjectId_ != projectId )
                                return;
                            requestPreviewBuild( true, true );
                            ensureLspForActiveDocument();
                        } );
                    }
                    emit pythonEnvironmentRepairFinished( projectKey, success, message );
                } );
    }
}

bool WorkspaceController::repairPythonEnvironment( const QString& projectKey )
{
    if( pythonEnv_ == nullptr )
        return false;
    const auto it = damagedPythonEnvironments_.constFind( projectKey );
    if( it == damagedPythonEnvironments_.constEnd() )
        return false;

    return pythonEnv_->repairProjectEnvironmentAsync( projectKey, it->projectRoot,
                                                       it->environmentPath );
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
    if( shuttingDown_ )
        return;

    QTextView* view = activeView_;
    DocumentContext* context = contextFor( view );
    if( view == nullptr || context == nullptr || context->path.isEmpty() )
        return;

    // 라우팅은 아래 두 가드보다 **앞에서** 갈라야 한다. 내장 렌더러 경로는 파이썬도
    // Sphinx 프로젝트도 필요 없는데, 아래는 그 둘이 없으면 조용히 반환한다. 뒤에 두면
    // 런타임 준비 전이나 프로젝트 밖의 .md 는 영영 프리뷰가 뜨지 않는다.
    if( routeFor( *context ) == PreviewRoute::MarkdownJs )
    {
        renderMarkdownJs( *context, immediate, forceRebuild );
        return;
    }

    if( previewController_ == nullptr || pythonEnv_ == nullptr )
        return;

    // 런타임이 준비되기 전에는 조용히 넘어간다. 준비되면 다시 호출된다.
    if( !pythonEnv_->isReady() )
        return;

    const SphinxProject* project = lookupProject( context->projectId );
    if( project == nullptr )
        return;   // 어느 프로젝트에도 속하지 않는 .rst 는 가상 프로젝트가 받는다.

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
    request.stubDoxygenForShadow = previewStubDoxygenWhileTyping_;
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
    setPreviewStatus( tr( "프리뷰 준비 중..." ),
                      previewOverallPermille( PreviewPhase::Prepare, 0, 0 ) );

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
        // conf.py 정규식이 myst 라고 봤는데 빌더가 이 파일을 원본으로 읽지 않았다.
        // ast 로 읽는 쪽이 진실이므로 그 판정을 정정해 기억한다. 프로젝트 단위라
        // 이 2.5초는 그 프로젝트에서 한 번만 낸다.
        if( filekinds::hasExtension( result.sourceFile, filekinds::markdownExtensions() )
            && !result.projectId.isEmpty() )
        {
            mystDeniedProjects_.insert( result.projectId );
            emit logMessage( QStringLiteral( "[md] %1 -> MarkdownJs (빌더가 원본으로 읽지 않음)" )
                                .arg( QFileInfo( result.sourceFile ).fileName() ) );
        }

        emit logMessage( tr( "프리뷰를 만들 수 없는 파일입니다(이 프로젝트의 원본이 아님): %1" )
                            .arg( QFileInfo( result.sourceFile ).fileName() ) );
        setPreviewStatus( {} );
        return;
    }

    // data-mrr-src 인덱스를 실제 경로로 되돌리려면 빌더가 준 순서를 그대로 쓴다.
    setPreviewSources( result.sources );
    previewPrimaryPath_ = result.sourceFile;

    showPreviewHtml( result.htmlPath, result.projectId + QLatin1Char( '\x1f' ) + result.primaryDocname,
                     result.serial );
}

void WorkspaceController::setPreviewStatus( const QString& text, const int permille )
{
    // 문구가 같아도 진행도가 움직이면 내보낸다. 분모를 모르는 구간(문구가 그대로인
    // 채로 진행도만 올라가는 구간)에서 막대가 멈춘 것처럼 보이지 않게 한다.
    if( previewStatus_ == text && previewPermille_ == permille )
        return;

    previewStatus_   = text;
    previewPermille_ = permille;
    emit previewStatusChanged( text, !text.isEmpty(), permille );
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

    // 출력 디렉터리가 프로젝트당 하나로 고정이라 같은 문서의 URL 은 매번 같다.
    // 그대로 다시 load() 하면 Chromium 이 이전 내용을 캐시에서 낼 수 있으므로
    // 빌드 일련번호를 쿼리에 실어 다른 URL 로 만든다. file:// 에서 쿼리는 파일
    // 탐색에 쓰이지 않고, _static 같은 상대 경로 해석에도 영향이 없다.
    QUrl url = QUrl::fromLocalFile( htmlPath );
    url.setQuery( QStringLiteral( "b=%1" ).arg( buildSerial ) );

    // **똑같은 URL 을 이미 요청해 두고 아직 로드 중이면** 다시 부르지 않는다.
    // 다시 부르면 Chromium 이 진행 중인 로드를 버리고 처음부터 시작하므로,
    // 6MB 짜리 페이지에서는 방금 한 1.4초가 통째로 버려진다.
    //
    // 위쪽 가드로는 이 경우를 잡을 수 없다. 그것은 previewLoadedOk_ 를 보는데
    // 로드 중에는 아직 false 이기 때문이다. 그리고 previewLoadedOk_ 만으로는
    // "로드 중" 과 "끝났지만 실패" 를 구분할 수 없다 — 구분하지 않으면 실패한
    // 로드를 같은 URL 로 다시 시도할 수 없게 된다. 그래서 별도 플래그가 필요하다.
    //
    // 세션 복원 중에는 setActiveDocument 와 sigFileOpened 가 같은 문서를 두 번
    // 요청하므로 실제로 이 경로를 탄다(빌드는 sameOutcomeAs 로 접히지만, 지난
    // 산출물을 그대로 올리는 경로는 빌드를 거치지 않아 따로 막아야 한다).
    if( previewLoadInFlight_ && previewUrl_ == url )
    {
        traceP( "preview.load.skip",
               QStringLiteral( "in-flight %1" ).arg( htmlInfo.fileName() ) );
        // 상태 표시는 그대로 둔다. 로드가 정말 진행 중이므로 지우면 안 된다.
        return;
    }

    previewShownSize_ = size;
    previewShownMTimeMs_ = mtimeMs;

    const auto loadFullPage = [this, &url, &htmlPath, size] {
        traceP( "preview.load.begin",
               QStringLiteral( "%1 %2KB" ).arg( QFileInfo( htmlPath ).fileName() ).arg( size / 1024 ) );
        setPreviewStatus( tr( "프리뷰 로딩 중..." ),
                          previewOverallPermille( PreviewPhase::Load, 0, 100 ) );
        previewUrl_ = url;
        previewLoadedOk_ = false;
        previewLoadInFlight_ = true;
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
    // 본문을 갈아 끼울 것인가, 제자리에서 고칠 것인가. 기본은 고치는 쪽이고
    // (PreviewBridge::requestHotSwap 주석), 설정은 UI 없이 되돌리는 스위치다.
    const bool allowMorph =
        AppSettings().value( QStringLiteral( "preview/domMorph" ), true ).toBool();
    previewBridge_->requestHotSwap( html, baseUrl, allowMorph, ++hotSwapToken_ );
}

bool WorkspaceController::previewUrlIsOurs() const
{
    // qrc: 를 반드시 포함한다. Markdown 셸이 거기서 오고 QUrl::isLocalFile() 은
    // scheme=="file" 만 참이므로, 그것만 보면 md 프리뷰는 allowRemoteContent 를
    // 껐다 켜도 다시 읽히지 않는다(한 번 차단된 CDN 스크립트가 영영 안 살아난다).
    return previewUrl_.isLocalFile() || previewUrl_.scheme() == QLatin1String( "qrc" );
}

void WorkspaceController::showPreviewShell( const QString& documentPath )
{
    if( previewView_ == nullptr )
        return;

    // data-mrr-src 는 단일 파일이라 항상 0 이다. syncPreviewFromEditor() 의
    // sourceIndexForPath() 가 0 을 찾으려면 이 목록이 반드시 이 문서여야 한다.
    setPreviewSources( { documentPath } );
    previewProcessedSources_.clear();
    previewPrimaryPath_ = documentPath;

    // Sphinx 핫스왑 판정의 입력을 여기서 끊는다.
    //
    // 남겨 두면 .rst A -> .md -> 같은 .rst A 로 돌아올 때 documentKey 가 그대로여서
    // sameDocument 가 참, <head> 서명이 그대로여서 sameHead 가 참, previewUrl_ 과
    // url() 이 둘 다 셸이어서 "사용자가 페이지에 머물렀다" 가 참이 된다. 그러면
    // showPreviewHtml() 이 핫스왑 분기로 가서 **.rst 본문을 Markdown 셸 페이지 안에
    // 갈아 끼운다.**
    previewDocumentKey_ = QStringLiteral( "md\x1f" ) + documentPath;
    previewHeadSignature_.clear();
    previewShownSize_ = -1;
    previewShownMTimeMs_ = -1;

    const QUrl url = markdownPreview_->shellUrl();

    // 셸은 문서와 무관하므로 .md <-> .md 전환에는 항해가 없다. 원문만 다시 밀면
    // 되고, 그 덕에 세션에서 받아 둔 CDN 자산이 살아 있다.
    if( previewLoadedOk_ && previewBridge_ != nullptr && previewBridge_->isReady()
        && previewUrl_.path() == url.path() )
    {
        return;
    }
    if( previewLoadInFlight_ && previewUrl_.path() == url.path() )
        return;

    setPreviewStatus( tr( "프리뷰 로딩 중..." ),
                      previewOverallPermille( PreviewPhase::Load, 0, 100 ) );
    previewUrl_ = url;
    previewLoadedOk_ = false;
    previewLoadInFlight_ = true;
    if( previewBridge_ != nullptr )
        previewBridge_->resetReady();
    markdownPreview_->notifyShellReloaded();
    traceP( "preview.load.begin", QStringLiteral( "md-shell" ) );
    previewView_->load( url );
}

QString WorkspaceController::textForPreview( const DocumentContext& context ) const
{
    if( context.view.isNull() )
        return {};
    if( previewApplyUnsavedEdits_ || !context.view->isModified() )
        return context.view->text();

    // 설정을 끈 사용자는 "저장된 상태를 보고 싶다" 는 뜻이다. 재파싱 비용이라는
    // 원래 근거(Sphinx)는 여기서 사라지지만 그 의도는 남는다.
    QFile file( context.path );
    if( !file.open( QIODevice::ReadOnly ) )
        return context.view->text();
    return QString::fromUtf8( file.readAll() );
}

void WorkspaceController::renderMarkdownJs( DocumentContext& context, const bool immediate,
                                            const bool force )
{
    // 탭을 옮겼는데 프리뷰가 따라오지 않은 상태를 알아채는 기준이다. 채우지 않으면
    // setActiveDocument() 의 previewStale 판정이 매번 참이 되어 다시 요청한다.
    previewRequestedPath_ = context.path;

    // 이 .md 를 예전에 Sphinx 로 빌드해 남은 진단은 근거가 없어졌다. 지우지 않으면
    // 라우팅이 바뀐 문서의 옛 경고가 진단 표에 영영 남는다.
    diagnosticsStore_->replaceSourceForPath( QStringLiteral( "sphinx-build" ), context.path, {} );

    showPreviewShell( context.path );
    markdownPreview_->requestRender( context.path, textForPreview( context ), immediate, force );
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
    if( !firstApply && previewUrlIsOurs() )
    {
        setPreviewStatus( tr( "프리뷰 로딩 중..." ),
                          previewOverallPermille( PreviewPhase::Load, 0, 100 ) );
        previewLoadedOk_ = false;
        previewLoadInFlight_ = true;
        if( previewBridge_ != nullptr )
            previewBridge_->resetReady();
        // 셸을 다시 읽으면 아무것도 렌더되지 않은 상태로 돌아간다. 알려 주지 않으면
        // 마지막 push 해시가 남아 같은 원문이 "이미 보냈다" 로 걸러지고 프리뷰가
        // 빈 채로 남는다.
        markdownPreview_->notifyShellReloaded();
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

    // 증분이 켜져 있으면 sigDocumentEdited 경로가 이미 보냈다. 여기서 전문을 또
    // 보내면 677KB 문서에서 키 입력마다 692KB 를 두 번 쓰는 셈이 된다.
    if( client->supportsIncrementalSync() )
        return;

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

    // text() 는 문서 전체를 QString 으로 복사한다. UTF-16 이라 677KB 문서에서 한 번에
    // 1.3MB 다. 예전에는 폴백과 didChange 가 각각 불러 두 번 복사했다.
    const QString documentText = activeView_->text();

    // 폴백을 먼저 내보낸다. Esbonio 가 데워지기 전에도 개요가 비어 있지 않게.
    emit documentOutlineReady( context->path, parseDocumentOutline( documentText, context->path ) );

    // LSP 가 살아 있으면 더 정확한 결과로 덮어쓴다.
    if( LspClient* client = lspPool_->clientFor( context->projectId );
        client != nullptr && client->isRunning() && context->syncedToServer )
    {
        // 증분이 켜져 있으면 서버 사본은 이미 최신이다.
        if( !client->supportsIncrementalSync() )
            client->didChange( context->path, documentText );
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

void WorkspaceController::scheduleDiagnosticMarksRefresh( const QString& normalizedPath )
{
    // 한 빌드가 처리 문서마다 pathChanged 를 낸다. 그 하나하나가 열린 문서를
    // 훑어 Scintilla 인디케이터를 통째로 지우고 다시 칠하므로, 같은 회전에 온
    // 것들을 모아 문서별로 **한 번만** 칠한다.
    if( normalizedPath.isEmpty() )
        return;

    pendingDiagnosticMarkPaths_.insert( normalizedPath );
    if( pendingDiagnosticMarkPaths_.size() > 1 )
        return;   // 이미 예약되어 있다

    QTimer::singleShot( 0, this, [this] {
        const QSet< QString > paths = std::move( pendingDiagnosticMarkPaths_ );
        pendingDiagnosticMarkPaths_.clear();
        for( const QString& path : paths )
            refreshDiagnosticMarks( path );
    } );
}

void WorkspaceController::refreshDiagnosticMarks( const QString& normalizedPath )
{
    if( diagnosticsStore_ == nullptr )
        return;

    // 호출 횟수와 한 번의 값을 함께 봐야 진단 폭주가 표(diag.table) 쪽인지
    // 스퀴글 쪽인지 가른다.
    const mrst::PhaseSpan span( "diag.marks" );

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

PathIndex* WorkspaceController::pathIndex()
{
    return pathIndex_;
}

QString WorkspaceController::workspaceRoot() const
{
    return registry_->workspaceRoot();
}

QString WorkspaceController::activeProjectId() const
{
    return activeProjectId_;
}

bool WorkspaceController::activeDocumentIsStandalone() const
{
    // contextFor() 는 비-const 라 여기서 쓸 수 없다. 텍스트 뷰가 아닌 탭
    // (이미지·PDF)과 탭이 하나도 없는 상태가 모두 여기로 떨어져 false 가 된다.
    const auto it = documents_.constFind( activeView_.data() );
    if( it == documents_.constEnd() )
        return false;

    const DocumentContext& context = it.value();
    if( !context.projectId.isEmpty() && !context.isVirtual )
        return false;   // conf.py 를 가진 실제 프로젝트에 속한다

    // 가상 프로젝트도 "없음" 으로 센다. 그 srcdir 은 임시 디렉터리가 아니라
    // 원본 파일이 있던 실제 디렉터리라서(solVirtualProjectMgr.hpp 참고), 프로젝트
    // 개요를 만들면 그 폴더의 무관한 문서까지 재귀로 늘어놓는다 — 문서 하나짜리
    // 목록이 아니다.
    return filekinds::hasExtension( context.path, filekinds::restructuredTextExtensions() )
           || filekinds::hasExtension( context.path, filekinds::markdownExtensions() );
}

DiagnosticsStore* WorkspaceController::diagnostics() const
{
    return diagnosticsStore_;
}

void WorkspaceController::setWorkspaceRoot( const QString& root )
{
    const QString previousRoot = registry_->workspaceRoot();
    registry_->setWorkspaceRoot( root );
    const QString currentRoot = registry_->workspaceRoot();
    if( envResolver_ != nullptr )
        envResolver_->setWorkspaceRoot( currentRoot );

    if( previousRoot == currentRoot )
        return;

    // 프로젝트 경계가 바뀌면 실행 중인 서비스와 캐시도 함께 끊는다. 탭만 닫고
    // 이것들을 남기면 이전 워크스페이스의 LSP 진단과 프리뷰 완료 신호가 새
    // 워크스페이스 화면을 다시 채울 수 있다.
    if( previewController_ != nullptr )
        previewController_->cancelImmediately();
    if( markdownPreview_ != nullptr )
    {
        markdownPreview_->cancel();
        markdownPreview_->notifyShellReloaded();
    }
    if( projectBuildTask_ != nullptr )
        projectBuildTask_->cancel();
    if( lspPool_ != nullptr )
    {
        lspPool_->setPinnedProject( {} );
        lspPool_->stopAll();
    }
    if( virtualProjects_ != nullptr )
        virtualProjects_->cleanup();
    if( diagnosticsStore_ != nullptr )
        diagnosticsStore_->clear();

    mystDeniedProjects_.clear();
    pendingDiagnosticMarkPaths_.clear();
    setPreviewSources( {} );
    previewProcessedSources_.clear();
    previewRequestedPath_.clear();
    previewPrimaryPath_.clear();
    previewDocumentKey_.clear();
    previewHeadSignature_.clear();
    previewShownSize_ = -1;
    previewShownMTimeMs_ = -1;
    previewUrl_.clear();
    previewLoadedOk_ = false;
    previewLoadInFlight_ = false;
    pendingFullLoadPath_.clear();
    pendingFullLoadUrl_.clear();
    ++previewGateGeneration_;
    ++hotSwapToken_;
    if( previewSyncRetry_ != nullptr )
        previewSyncRetry_->stop();
    setPreviewStatus( {} );

    activeProjectId_.clear();
    lspState_.clear();
    projectOutlineProjectId_.clear();
    ++outlineGeneration_;
    if( outlineDebounce_ != nullptr )
        outlineDebounce_->stop();
    if( completions_ != nullptr )
        completions_->setActiveProject( {}, {}, currentRoot );
    if( glossary_ != nullptr )
        glossary_->setActiveProjectId( {} );
    if( substitutions_ != nullptr )
        substitutions_->setActiveProjectId( {} );

    for( DocumentContext& context : documents_ )
    {
        context.projectId.clear();
        context.isVirtual = false;
        context.syncedToServer = false;
        context.nudgedInitialBuild = false;
        if( !context.view.isNull() )
            context.view->setDiagnosticMarks( {} );
    }

    emit activeProjectChanged( {}, false );
    emit projectsChanged( 0 );
    emit projectOutlineReady( {}, {}, 0 );
    emit outlineCleared( tr( "열린 문서가 없습니다." ) );

    // 빠른 파일 열기는 첫 호출 때 전체 트리를 기다리지 않아야 한다. 프로젝트
    // 스캔과 마찬가지로 워크스페이스를 정한 직후 백그라운드에서 인덱스를 만든다.
    // 닫을 때는 이전 루트의 늦은 배치가 다음 워크스페이스로 넘어오지 않게 버린다.
    if( pathIndex_ != nullptr )
    {
        if( currentRoot.isEmpty() )
            pathIndex_->clear();
        else
            pathIndex_->ensure( currentRoot );
    }

    if( currentRoot.isEmpty() )
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
    // conf.py 를 고쳐 myst 를 켰을 수 있다. 정정 기록을 들고 있으면 그 프로젝트는
    // 재스캔 뒤에도 내장 렌더러로 남는다.
    mystDeniedProjects_.clear();
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
    previewStubDoxygenWhileTyping_ =
        settings.value( QStringLiteral( "preview/stubDoxygenWhileTyping" ), true ).toBool();
    // 0 은 "제한 없음" 이다. 빌더는 음수를 그 뜻으로 받는다.
    const int maxReadMs = settings.value( QStringLiteral( "preview/unsavedEditMaxReadMs" ), 2000 ).toInt();
    previewUnsavedMaxReadMs_ = ( maxReadMs > 0 ) ? maxReadMs : -1;

    // 수식 렌더러를 바꾸면 셸을 다시 읽어야 한다. 라이브러리는 한 번 로드되면
    // 내릴 수 없어서 재렌더로는 갈 수 없다. allowRemoteContent 의 선례와 같은
    // 판단이지만, 그것과 달리 **Sphinx 경로에는 영향이 없다** — 이 값은 Sphinx
    // 출력과 무관하므로 md 프리뷰가 떠 있을 때만 손댄다.
    const QString mathRenderer =
        settings.value( QStringLiteral( "preview/mathRenderer" ), QStringLiteral( "katex" ) ).toString();
    if( previewMathRenderer_ != mathRenderer )
    {
        const bool firstApply = previewMathRenderer_.isEmpty();
        previewMathRenderer_ = mathRenderer;
        DocumentContext* context = contextFor( activeView_ );
        if( !firstApply && context != nullptr && routeFor( *context ) == PreviewRoute::MarkdownJs )
        {
            showPreviewShell( context->path );
            markdownPreview_->requestRender( context->path, textForPreview( *context ),
                                            /*immediate=*/true, /*force=*/true );
        }
    }

    // 테마는 마지막에 본다. 위에서 프리뷰 설정을 다 반영한 뒤여야, 여기서
    // 일어나는 재빌드가 새 설정으로 나간다.
    applyVirtualProjectTheme();

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
    refreshSubstitutions( true );
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

    // 편집 구간을 그대로 서버에 흘린다. 디바운스하지 않는다 — 증분 동기화는 편집을
    // 하나라도 빠뜨리면 서버 사본이 어긋나고 그 뒤 진단·완성 위치가 전부 밀린다.
    // 프레임 하나가 수백 바이트라 전문 전송(677KB 문서에서 692KB)과 비교가 되지 않는다.
    connect( view, &QTextView::sigDocumentEdited, this,
             [ this, view ]( int startLine, int startColumn, int oldEndLine, int oldEndColumn,
                             const QByteArray& newText ) {
                 DocumentContext* context = contextFor( view );
                 if( context == nullptr || !context->syncedToServer || context->path.isEmpty() )
                     return;   // 아직 didOpen 전이다. 곧 전문이 나가므로 여기서 보낼 것이 없다.

                 LspClient* client = lspPool_->clientFor( context->projectId );
                 if( client == nullptr || !client->isRunning() || !client->supportsIncrementalSync() )
                     return;   // 협상 실패. syncDocumentToServer 의 전문 경로가 맡는다.

                 client->didChangeIncremental( context->path, startLine, startColumn, oldEndLine,
                                               oldEndColumn, newText );
             } );

    // 편집 중에는 디바운스된 프리뷰 재빌드 + LSP 문서 동기화.
    connect( view, &QTextView::sigTextEdited, this, [this, view] {
        if( activeView_ != view )
            return;

        DocumentContext* context = contextFor( view );
        if( context != nullptr
            && filekinds::hasExtension( context->path, filekinds::markdownExtensions() ) )
        {
            // 한글 IME 는 기존 조합 문자를 먼저 지운 뒤 새 조합 문자를 넣는다.
            // sigTextEdited 에서 곧바로 전문을 복사하면 입력 이벤트 중간의, 마지막
            // 글자가 빠진 문자열이 Markdown 디바운스 큐에 남을 수 있다. 이벤트가
            // 끝난 다음 현재 버퍼를 다시 읽어 삭제/삽입 묶음을 한 요청으로 접는다.
            if( !markdownEditRefreshQueued_ )
            {
                markdownEditRefreshQueued_ = true;
                const QPointer< QTextView > editedView( view );
                QTimer::singleShot( 0, this, [this, editedView] {
                    markdownEditRefreshQueued_ = false;
                    if( editedView.isNull() || activeView_ != editedView )
                        return;
                    requestPreviewBuild( false );
                } );
            }
        }
        else
        {
            requestPreviewBuild( false );
        }

        if( context != nullptr )
            syncDocumentToServer( *context, false );
        outlineDebounce_->start();
    } );

    // 밖에서 바뀐 파일을 다시 불러왔다. 편집과 달리 **배경 탭에서도 일어난다** —
    // 그래서 활성 여부를 먼저 따지지 않는다. 서버가 든 사본이 낡은 채로 남으면
    // 그 탭으로 돌아가는 순간 엉뚱한 줄에 진단이 붙는다.
    connect( view, &QTextView::sigFileReloadedFromDisk, this, [this, view]( const QString& ) {
        DocumentContext* context = contextFor( view );
        if( context == nullptr )
            return;

        syncDocumentToServer( *context, false );
        if( activeView_ != view )
            return;
        requestPreviewBuild( false );
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
        completions_->setActiveProject( QString{}, QString{}, QString{} );
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
        // 소스 루트는 **값으로** 넘긴다. registry_ 가 돌려주는 포인터는 다음
        // 스캔에 무효화되므로 조율자가 들고 있으면 안 된다.
        const SphinxProject* activeProject = lookupProject( activeProjectId_ );
        completions_->setActiveProject( activeProjectId_,
                                       activeProject != nullptr
                                           ? toQString( activeProject->sourcePath )
                                           : QString{},
                                       registry_->workspaceRoot() );
        emit activeProjectChanged( activeProjectId_, context->isVirtual );
        emit logMessage( tr( "활성 프로젝트: %1" )
                            .arg( activeProjectId_.isEmpty() ? unresolvedProjectLabel() : activeProjectId_ ) );
    }

    // 요약 패널이 어느 탭을 앞에 둘지 여기서 알린다. 아래 중복 호출 가드보다
    // **앞**에 둔다 — 단독 `.md` 는 projectId 가 계속 빈 문자열이어서 스캔이
    // 끝난 뒤의 재호출이 그 가드에 걸리는데, 그때가 바로 답이 확정되는 순간이다.
    //
    // 스캔이 도는 동안에는 내보내지 않는다. 그때는 실제 프로젝트에 속한 문서도
    // projectId 가 비어 있어(resolveProject 가 조기 반환한다) 단독 문서와
    // 구별되지 않는다. 스캔이 끝나면 scanFinished 가 소속을 다시 정한 뒤 이
    // 함수를 다시 부르므로, 한 박자 늦게 정확한 답이 나간다.
    if( !registry_->isScanning() )
        emit activeDocumentResolved( activeDocumentIsStandalone() );

    if( !projectChanged && !documentChanged && !previewStale )
        return;   // 같은 문서에 대한 중복 호출

    // `.md` 는 소속과 conf.py 에 따라 프리뷰를 만드는 쪽이 갈린다. 화면만 보고는
    // 어느 쪽이 돌았는지 알 수 없으므로 판정을 남긴다. 중복 호출 가드 뒤에 두어
    // 새 탭 추가 과정의 두 번째 setActiveDocument 가 로그를 다시 채우지 않게 한다.
    if( filekinds::hasExtension( context->path, filekinds::markdownExtensions() ) )
    {
        emit logMessage( QStringLiteral( "[md] %1 -> %2" )
                            .arg( QFileInfo( context->path ).fileName(),
                                  routeFor( *context ) == PreviewRoute::Sphinx
                                      ? QStringLiteral( "Sphinx" )
                                      : QStringLiteral( "MarkdownJs" ) ) );
    }

    requestPreviewBuild( true );
    ensureLspForActiveDocument();

    outlineDebounce_->stop();
    refreshDocumentOutline();
    refreshProjectOutline( false );
    refreshGlossary( false );
    refreshSubstitutions( false );
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

void WorkspaceController::refreshSubstitutions( const bool force )
{
    if( shuttingDown_ || substitutions_ == nullptr )
        return;

    substitutions_->setActiveProjectId( activeProjectId_ );

    const SphinxProject* project = lookupProject( activeProjectId_ );
    if( project == nullptr )
        return;

    substitutions_->refresh( activeProjectId_, toQString( project->sourcePath ),
                            QString::fromStdString( project->rootDoc ),
                            toQString( project->confPath ), force );
}

// ── 사용자가 요청한 1회성 빌드 ────────────────────────────

bool WorkspaceController::isProjectBuildRunning() const
{
    return !projectBuildTask_.isNull() && projectBuildTask_->isRunning();
}

void WorkspaceController::cancelProjectBuild()
{
    if( !projectBuildTask_.isNull() )
        projectBuildTask_->cancel();
}

bool WorkspaceController::buildProject( const QString& projectId, const QString& builder,
                                        const QString& outputDirectory )
{
    if( shuttingDown_ )
        return false;

    if( isProjectBuildRunning() )
    {
        emit logMessage( tr( "빌드가 이미 돌고 있습니다. 끝난 뒤에 다시 요청하십시오." ) );
        return false;
    }
    if( !isValidSphinxBuilderName( builder ) || outputDirectory.trimmed().isEmpty() )
    {
        emit logMessage( tr( "빌더 이름이나 출력 위치가 올바르지 않습니다." ) );
        return false;
    }

    const SphinxProject* project = lookupProject( projectId );
    if( project == nullptr )
    {
        emit logMessage( tr( "빌드할 프로젝트를 찾을 수 없습니다 [%1]." ).arg( projectId ) );
        return false;
    }
    if( pythonEnv_ == nullptr || !pythonEnv_->isReady() )
    {
        emit logMessage( tr( "파이썬 런타임이 아직 준비되지 않았습니다. 잠시 뒤에 다시 요청하십시오." ) );
        return false;
    }

    projectBuild_ = {};
    projectBuild_.projectId = projectId;
    projectBuild_.builder = builder.trimmed();
    projectBuild_.outputDirectory = QDir::cleanPath( outputDirectory.trimmed() );
    projectBuild_.sourceDir = toQString( project->sourcePath );
    projectBuild_.confDir = toQString( project->rootPath );
    // make 모드는 `<출력 위치>/<하위 폴더>` 아래에 쓴다. 그 이름은 목표 이름과
    // 같지 않다 (latexpdf → latex/, info → texinfo/).
    const QString makeSubdirectory = sphinxMakeModeSubdirectory( projectBuild_.builder );
    projectBuild_.resultDirectory =
        makeSubdirectory.isEmpty()
            ? projectBuild_.outputDirectory
            : QDir( projectBuild_.outputDirectory ).absoluteFilePath( makeSubdirectory );

    if( !QDir().mkpath( projectBuild_.outputDirectory ) )
    {
        emit logMessage( tr( "출력 위치를 만들 수 없습니다: %1" ).arg( projectBuild_.outputDirectory ) );
        return false;
    }

    // 프리뷰와 같은 규칙이다 — 프로젝트가 정한 인터프리터로 돌려야 그 venv 의
    // 테마와 확장이 그대로 반영된다. 거기 Sphinx 가 없으면 내장 환경으로 한 번
    // 물러선다 (SphinxPreviewController::finishBuild 와 같은 판단).
    const ResolvedPythonEnv env = envResolver_->resolve( *project );
    projectBuild_.fallbackPython = pythonEnv_->pythonExe();

    emit projectBuildStarted( projectId, projectBuild_.builder );
    startProjectBuild( env.pythonExe.isEmpty() ? pythonEnv_->pythonExe() : env.pythonExe );
    return true;
}

void WorkspaceController::startProjectBuild( const QString& pythonExe )
{
    projectBuild_.pythonExe = pythonExe;
    const bool makeMode = isSphinxMakeModeTarget( projectBuild_.builder );

    UvTask::Request request;
    request.program = pythonExe;
    request.arguments = { QStringLiteral( "-X" ), QStringLiteral( "utf8" ),
                         QStringLiteral( "-m" ), QStringLiteral( "sphinx" ),
                         makeMode ? QStringLiteral( "-M" ) : QStringLiteral( "-b" ),
                         projectBuild_.builder };
    // srcdir 과 confdir 이 갈리는 배치(원본이 conf.py 옆이 아닌 경우)에서는
    // 알려 주어야 한다. 기본값은 srcdir 이라 그대로 두면 conf.py 를 못 찾는다.
    if( !projectBuild_.confDir.isEmpty() && projectBuild_.confDir != projectBuild_.sourceDir )
        request.arguments << QStringLiteral( "-c" ) << projectBuild_.confDir;
    request.arguments << projectBuild_.sourceDir << projectBuild_.outputDirectory;
    request.workingDirectory = projectBuild_.confDir;
    request.tag = QStringLiteral( "sphinx %1 %2" )
                      .arg( makeMode ? QStringLiteral( "-M" ) : QStringLiteral( "-b" ),
                           projectBuild_.builder );

    emit logMessage( tr( "빌드 시작: %1 → %2" )
                        .arg( projectBuild_.builder, projectBuild_.outputDirectory ) );

    auto* task = new UvTask( request, this );
    projectBuildTask_ = task;
    connect( task, &UvTask::outputLine, this, &WorkspaceController::logMessage );
    connect( task, &UvTask::failedToStart, this, [ this, task ]( const QString& message ) {
        emit logMessage( message );
        task->deleteLater();
        finishProjectBuild( -1, true, false, QString{} );
    } );
    connect( task, &UvTask::finished, this, [ this, task ]( const int exitCode, const bool crashed ) {
        const bool    cancelled = task->wasCancelled();
        const QString output = task->collectedOutput();
        task->deleteLater();
        finishProjectBuild( exitCode, crashed, cancelled, output );
    } );
    task->start();
}

void WorkspaceController::finishProjectBuild( const int exitCode, const bool crashed,
                                              const bool cancelled, const QString& output )
{
    const bool brokenPython = !cancelled && !projectBuild_.usedFallback
        && !projectBuild_.fallbackPython.isEmpty()
        && projectBuild_.pythonExe != projectBuild_.fallbackPython
        && pythonFailureIndicatesBrokenEnvironment( exitCode, crashed, output );

    // 프로젝트 venv 가 문서용이 아니라 애플리케이션용이면 Sphinx 가 없다.
    // 종료 코드로는 구분되지 않아(그냥 1이다) 출력을 본다.
    if( !cancelled && !projectBuild_.usedFallback && !projectBuild_.fallbackPython.isEmpty()
        && ( brokenPython || output.contains( QLatin1String( "No module named sphinx" ) ) ) )
    {
        projectBuild_.usedFallback = true;
        if( brokenPython )
        {
            if( const SphinxProject* project = lookupProject( projectBuild_.projectId );
                project != nullptr && envResolver_ != nullptr )
            {
                const QString reason = output.trimmed().isEmpty()
                                           ? tr( "프로젝트 Python을 실행할 수 없습니다 (종료 코드 %1)." )
                                                 .arg( exitCode )
                                           : output.trimmed();
                envResolver_->reportRuntimeFailure( *project, projectBuild_.pythonExe, reason );
            }
            emit logMessage( tr( "프로젝트 Python 환경이 손상되어 내장 환경으로 다시 시도합니다." ) );
        }
        else
        {
            emit logMessage( tr( "프로젝트 환경에 Sphinx 가 없어 내장 환경으로 다시 시도합니다." ) );
        }
        startProjectBuild( projectBuild_.fallbackPython );
        return;
    }

    const bool ok = !crashed && !cancelled && exitCode == 0;
    if( cancelled )
        emit logMessage( tr( "빌드를 취소했습니다." ) );
    else if( ok )
        emit logMessage( tr( "빌드 완료: %1" ).arg( projectBuild_.resultDirectory ) );
    else
        emit logMessage( tr( "빌드 실패 (종료 코드 %1). 로그를 확인하십시오." ).arg( exitCode ) );

    emit projectBuildFinished( projectBuild_.projectId, projectBuild_.builder,
                              projectBuild_.resultDirectory, ok, cancelled );
}

void WorkspaceController::requestCompletion()
{
    if( shuttingDown_ || activeView_.isNull() )
        return;

    // didChange 흘려보내기는 lspCompletionRequested 를 받는 쪽이 한다.
    // 자동 호출도 같은 길을 타야 두 목록이 같아진다.
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
    // 치환 정의도 마찬가지다. 편집 중인 문서의 정의는 버퍼에서 바로 읽지만
    // **다른 문서**에 방금 정의한 것은 저장을 거쳐야 목록에 들어온다.
    refreshSubstitutions( true );

    // 새 파일이 생겼을 수 있다. 스로틀이 있어 저장할 때마다 훑지는 않는다.
    if( pathIndex_ != nullptr && !registry_->workspaceRoot().isEmpty() )
        pathIndex_->invalidate( registry_->workspaceRoot() );
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

QString WorkspaceController::resolveVirtualProjectTheme() const
{
    const QString configured =
        AppSettings().value( QStringLiteral( "preview/virtualProjectTheme" ) ).toString().trimmed();
    if( !configured.isEmpty() )
        return configured;

    // "다른 프로젝트와 동일". 워크스페이스의 실제 프로젝트가 선언한 테마를 쓴다.
    //
    // 서로 다른 테마를 쓰는 프로젝트가 섞여 있으면 스캔 순서로 처음 만난 것을
    // 쓴다. 다수결은 쓰지 않는다 — 문서 하나뿐인 프로젝트가 본 문서 수백 개인
    // 프로젝트와 같은 한 표를 갖는다.
    for( const SphinxProject& project : registry_->projects() )
    {
        const std::string theme = readHtmlTheme( project.confPath );
        if( !theme.empty() )
            return QString::fromStdString( theme );
    }
    return {};
}

void WorkspaceController::applyVirtualProjectTheme()
{
    if( virtualProjects_ == nullptr || shuttingDown_ )
        return;

    const QString theme = resolveVirtualProjectTheme();
    if( virtualProjects_->htmlTheme() == theme )
        return;

    // 활성 문서가 가상인지 **먼저** 본다. 아니라면 임시 디렉터리를 지워도 화면에
    // 보이는 것은 없으므로, 실제 프로젝트의 진행 중인 빌드를 괜히 끊지 않는다
    // (설정을 적용하는 순간에 그것이 돌고 있을 수 있다).
    bool activeIsVirtual = false;
    for( const DocumentContext& context : documents_ )
    {
        if( context.isVirtual && context.view == activeView_ )
        {
            activeIsVirtual = true;
            break;
        }
    }

    // 진행 중인 빌드는 곧 지워질 임시 디렉터리를 confdir 로 쓰고 있다.
    if( activeIsVirtual && previewController_ != nullptr )
        previewController_->cancel();

    virtualProjects_->setHtmlTheme( theme );

    // 가상 프로젝트 핸들이 방금 통째로 버려졌다. 그것에 배정돼 있던 문서의
    // 배정을 풀어 두면 아래 setActiveDocument() 가 새 conf.py 로 다시 만든다.
    // (scanFinished 의 프로젝트 재배정과 같은 절차다.)
    for( DocumentContext& context : documents_ )
    {
        if( !context.isVirtual )
            continue;

        if( !context.projectId.isEmpty() && lspPool_ != nullptr )
            lspPool_->stopProject( context.projectId );
        context.projectId.clear();
        context.isVirtual = false;
        context.syncedToServer = false;
        context.nudgedInitialBuild = false;
    }

    if( !activeIsVirtual )
        return;

    // activeProjectId_ 를 비워야 setActiveDocument() 가 "프로젝트가 바뀌었다" 로
    // 보고 프리뷰를 다시 만든다. 그러지 않으면 화면은 옛 테마로 남는다.
    activeProjectId_.clear();
    setActiveDocument( activeView_ );
}

const SphinxProject* WorkspaceController::lookupProject( const QString& projectId ) const
{
    if( const SphinxProject* project = registry_->findById( projectId ) )
        return project;
    return virtualProjects_->findById( projectId );
}

PreviewRoute WorkspaceController::routeFor( const DocumentContext& context ) const
{
    if( context.path.isEmpty() )
        return PreviewRoute::None;

    // .md 가 아니면 지금까지와 같다.
    if( !filekinds::hasExtension( context.path, filekinds::markdownExtensions() ) )
        return PreviewRoute::Sphinx;

    // 어느 프로젝트에도 속하지 않는 파일. Sphinx 로 보낼 conf.py 가 없다.
    if( context.projectId.isEmpty() )
        return PreviewRoute::MarkdownJs;

    // 빌더가 이 프로젝트에서 .md 를 원본으로 읽지 않는다고 이미 알려 주었다.
    // (1차 판정인 conf.py 정규식이 틀렸던 경우다.)
    if( mystDeniedProjects_.contains( context.projectId ) )
        return PreviewRoute::MarkdownJs;

    const SphinxProject* project = lookupProject( context.projectId );
    if( project != nullptr && project->mystMarkdown )
        return PreviewRoute::Sphinx;

    return PreviewRoute::MarkdownJs;
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
