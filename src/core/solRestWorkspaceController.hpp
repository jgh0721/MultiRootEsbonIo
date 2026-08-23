#pragma once

#include "solRestOutlineService.hpp"
#include "solSphinxDiagnostics.hpp"
#include "solSphinxScanner.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QTextView;
class QTimer;
class QWebEngineView;

namespace mrst {

class CompletionCoordinator;
class DiagnosticsStore;
class GlossaryIndex;
class LspClient;
class LspServerPool;
class MarkdownPreviewController;
class PathIndex;
class PreviewBridge;
class ProjectRegistry;
class PythonEnvManager;
class PythonEnvResolver;
struct ResolvedPythonEnv;
class SphinxPreviewController;
class SubstitutionIndex;
class VirtualProjectManager;
struct PreviewBuildResult;
struct PreviewBuildRequest;

/// 열린 문서 하나에 대한 Sphinx 관점의 상태.
///
/// MainWindow 는 탭/툴바/로딩 표시를 관리하고, 이 구조체는 "이 문서가 어느
/// 프로젝트에 속하며 LSP 서버와 동기화되었는가"만 담는다.
struct DocumentContext
{
    QPointer< QTextView >               view;
    QString                             path;                    ///< 정규화된 절대경로
    QString                             projectId;               ///< 해석 전에는 빈 문자열
    bool                                isVirtual = false;
    bool                                syncedToServer = false;
    /// 초기 빌드를 유발하는 synthetic didSave 를 이미 보냈는가.
    /// Esbonio 는 didOpen 만으로는 빌드하지 않아 진단이 나오지 않는다.
    bool                                nudgedInitialBuild = false;
};

/// 이 문서의 프리뷰를 무엇이 만드는가.
///
/// `.md` 는 두 갈래로 갈린다. myst-parser 를 켠 실제 Sphinx 프로젝트에 속하면
/// 그 프로젝트의 conf.py·테마·확장·상호참조가 그대로 반영되는 Sphinx 빌드가 낫고,
/// 그렇지 않은 `.md`(소속 없는 파일, myst 없는 프로젝트의 README 류)는 Sphinx 가
/// 아예 원본으로 읽지 않으므로 내장 렌더러가 유일한 길이다.
enum class PreviewRoute
{
    None,         ///< 프리뷰를 만들 수 없다 (문서 없음)
    Sphinx,
    MarkdownJs
};

/// MainWindow 와 Sphinx/Esbonio 서비스 계층 사이의 조율자.
///
/// 의존 방향은 단방향이다: MainWindow -> WorkspaceController -> 서비스들.
/// MainWindow 는 LSP 풀이나 프리뷰 프로세스를 직접 알지 못한다.
class WorkspaceController final : public QObject
{
    Q_OBJECT

public:
    explicit WorkspaceController( QObject* parent = nullptr );
    ~WorkspaceController() override;

    /// 프리뷰가 그려질 위젯을 주입한다. 컨트롤러는 소유권을 갖지 않는다.
    void                                setPreviewView( QWebEngineView* view );
    /// Python 런타임 제공자를 주입한다. 소유권 없음.
    void                                setPythonEnvironment( PythonEnvManager* manager );

    void                                setWorkspaceRoot( const QString& root );
    [[nodiscard]] QString               workspaceRoot() const;
    [[nodiscard]] ProjectRegistry*      projectRegistry() const;

    void                                rescanProjects();
    void                                reloadSettings();
    void                                shutdown();

    /// 종료 의사만 표시한다. 실제 정리는 shutdown() 이 한다.
    ///
    /// closeEvent 의 저장 확인 루프가 setCurrentIndex() 로 onTabChanged 를
    /// 일으켜 **종료 도중에** 프리뷰 빌드와 LSP 프로세스를 새로 띄우고,
    /// 개요(수백 문서)·용어집(수천 문서) 스캔까지 던진다. shuttingDown_ 가
    /// 그것을 전부 막지만 지금은 shutdown() 에서야 켜져서 이미 늦다.
    void                                beginShutdown();
    /// 사용자가 저장 확인에서 취소를 눌러 종료가 되돌아갔을 때.
    void                                endShutdown();

    // ── 문서 수명주기 (MainWindow 의 탭 배관에서 호출) ──
    void                                attachDocument( QTextView* view );
    void                                detachDocument( QTextView* view );
    void                                setActiveDocument( QTextView* view );
    void                                notifyDocumentSaved( QTextView* view );

    /// 세션 복원처럼 탭이 무더기로 열리는 구간을 감싼다. 그 사이의
    /// setActiveDocument() 는 대상만 기억해 두고, endBatchRestore() 에서
    /// **마지막 한 번만** 실제로 반영한다.
    ///
    /// 왜 필요한가: MainWindow::addViewTab() 이 탭마다 setActiveDocument() 를
    /// 부르므로, 복원 순서상 **첫 프리뷰 빌드가 0번 탭 것**이 되고 사용자가 볼
    /// 활성 탭 빌드는 그것이 끝난 뒤에야 시작한다. 순수 낭비 한 벌이다.
    /// setActiveDocument() 의 기존 중복 가드는 "같은 문서 재호출" 만 잡아서
    /// 이 상황을 표현할 수 없다.
    void                                beginBatchRestore();
    void                                endBatchRestore();

    [[nodiscard]] QString               activeProjectId() const;
    [[nodiscard]] DiagnosticsStore*     diagnostics() const;

    /// 활성 문서의 프리뷰를 다시 빌드한다.
    /// immediate=false 면 디바운스(편집 중), true 면 즉시(저장/탭 전환).
    ///
    /// forceRebuild=true 는 "입력이 안 바뀌었어도 반드시 빌드한다" 는 뜻이다.
    /// 변경 감지 게이트(preview/skipUnchangedBuild)를 우회하는 유일한 통로이고,
    /// 사용자가 메뉴로 명시적으로 요청했을 때만 쓴다. 게이트가 놓치는 입력이
    /// 있을 수 있으므로 이 탈출구가 반드시 있어야 한다.
    void                                requestPreviewBuild( bool immediate = false,
                                                             bool forceRebuild = false );

    /// Ctrl+Space. 트리거 문자 없이 지금 캐럿 위치에서 자동완성을 연다.
    void                                requestCompletion();

signals:
    void                                logMessage( const QString& text );
    void                                projectsChanged( int count );
    void                                activeProjectChanged( const QString& projectId, bool isVirtual );
    void                                navigateRequested( const QString& path, int line, int column );
    void                                diagnosticsChanged( const QString& source,
                                                            const QVector< DiagnosticEntry >& entries );
    void                                missingDependenciesDetected( const QString& projectId,
                                                                     const QStringList& distributions,
                                                                     const QStringList& themes );
    void                                lspStatusChanged( const QString& projectId, const QString& state );
    /// 프리뷰가 준비되는 동안의 상태. busy=false 면 text 는 비어 있고 표시를 지운다.
    /// 빌드뿐 아니라 HTML 로드 구간까지 덮는다 — 큰 문서는 빌드가 끝난 뒤에도
    /// WebEngine 이 읽는 데 시간이 걸려서, 그 사이가 비어 있으면 고장으로 보인다.
    void                                previewStatusChanged( const QString& text, bool busy );

    // ── 개요 ──
    /// 활성 문서의 섹션 개요. 먼저 정규식 폴백으로 한 번, LSP 응답이 오면 다시.
    void                                documentOutlineReady( const QString& path,
                                                              const QVector< OutlineSymbol >& symbols );
    /// 프로젝트의 문서 목록과 각 문서의 개요. truncated 는 상한에 걸려 잘린 개수.
    void                                projectOutlineReady( const QString& projectId,
                                                             const QVector< OutlineDocumentEntry >& documents,
                                                             int truncated );
    void                                outlineCleared( const QString& reason );

private:
    [[nodiscard]] DocumentContext*      contextFor( QTextView* view );
    void                                resolveProject( DocumentContext& context );
    /// 실제 프로젝트를 먼저 찾고, 없으면 가상 프로젝트에서 찾는다.
    [[nodiscard]] const SphinxProject*  lookupProject( const QString& projectId ) const;
    /// 이 문서의 프리뷰를 Sphinx 가 만드는가, 내장 Markdown 렌더러가 만드는가.
    [[nodiscard]] PreviewRoute          routeFor( const DocumentContext& context ) const;
    /// 가상 프로젝트의 합성 conf.py 가 쓸 `html_theme`.
    ///
    /// 설정이 비어 있으면("다른 프로젝트와 동일") 워크스페이스의 실제 프로젝트
    /// conf.py 에서 찾아 온다. 아무것도 못 찾으면 빈 문자열 —
    /// VirtualProjectManager 가 alabaster 로 물러선다.
    [[nodiscard]] QString               resolveVirtualProjectTheme() const;
    /// 위 값을 VirtualProjectManager 에 밀어 넣는다. 값이 바뀌었으면 가상
    /// 프로젝트로 렌더 중이던 문서를 새 conf.py 로 다시 잡는다.
    void                                applyVirtualProjectTheme();
    void                                logProjectList();
    void                                onPreviewFinished( const PreviewBuildResult& result );
    [[nodiscard]] QString               writeShadowCopy( QTextView* view, const QString& path ) const;
    /// 내장 Markdown 렌더러용 셸 페이지를 (다시) 로드한다.
    ///
    /// 항해는 이 함수와 showPreviewHtml() 두 곳으로만 한다. 그러지 않으면
    /// previewLoadInFlight_ / previewLoadedOk_ / 브리지 ready 상태가 어긋난다.
    void                                showPreviewShell( const QString& documentPath );
    /// 프리뷰가 **우리가 띄운** 페이지인가.
    ///
    /// 사용자가 프리뷰 안에서 외부 링크를 눌러 이동한 경우를 제외하려는 판정이다.
    /// qrc: 를 반드시 포함해야 한다 — Markdown 셸이 qrc 에서 오고,
    /// QUrl::isLocalFile() 은 scheme=="file" 만 참이다. 그것만 보면 md 프리뷰는
    /// allowRemoteContent 를 껐다 켜도 다시 읽히지 않아, 한 번 차단된 CDN
    /// 스크립트가 영영 안 살아난다.
    [[nodiscard]] bool                  previewUrlIsOurs() const;
    /// 내장 Markdown 렌더러로 이 문서를 그린다.
    void                                renderMarkdownJs( DocumentContext& context, bool immediate, bool force );
    /// 프리뷰에 넘길 원문. preview/applyUnsavedEdits 가 꺼져 있으면 디스크를 읽는다.
    [[nodiscard]] QString               textForPreview( const DocumentContext& context ) const;
    void                                showPreviewHtml( const QString& htmlPath, const QString& documentKey,
                                                          int buildSerial );
    /// 같은 문구를 반복 발신하지 않는다. text 가 비면 표시를 지운다.
    void                                setPreviewStatus( const QString& text );
    /// 프리뷰 페이지의 원격 리소스 접근 허용 여부를 설정에서 읽어 적용한다.
    /// 값이 바뀌었으면 이미 떠 있는 페이지를 다시 읽는다 (실패한 스크립트는
    /// 설정만 바꿔서는 다시 실행되지 않는다).
    void                                applyPreviewWebSettings();
    /// 입력이 안 바뀌었는지 워커 스레드에서 판정하고, 결과에 따라 빌드하거나
    /// 지난 산출물을 그대로 올린다.
    void                                tryServeFromLastBuild( const PreviewBuildRequest& request,
                                                               bool immediate );
    void                                onPreviewGateDecided( const PreviewBuildRequest& request,
                                                              bool immediate, bool changed );
    void                                refreshDiagnosticMarks( const QString& normalizedPath );

    // ── Esbonio ──
    // 지금은 활성 프로젝트 하나만 띄운다. 프로젝트당 하나씩 유지하는 풀은
    // 다음 단계에서 이 자리를 대체한다.
    void                                ensureLspForActiveDocument();
    void                                syncDocumentToServer( DocumentContext& context, bool forceOpen );
    void                                setLspStatus( const QString& state );
    void                                nudgeInitialBuild( const QString& projectId );
    /// 서버가 새로 떴을 때 그 프로젝트의 **모든** 열린 탭을 다시 didOpen 한다.
    /// 축출 후 재기동하면 서버에 문서 상태가 전혀 없으므로, 활성 탭 하나만
    /// 열면 나머지 탭의 진단이 조용히 사라진다.
    void                                reopenDocumentsForProject( const QString& projectId );

    // ── 개요 ──
    /// 활성 문서의 개요를 정규식 폴백으로 즉시 내보내고 LSP 에도 물어본다.
    void                                refreshDocumentOutline();
    /// 프로젝트 문서 목록을 작업 스레드에서 훑는다.
    /// force=false 면 같은 프로젝트에 대해 다시 돌지 않는다.
    void                                refreshProjectOutline( bool force );
    void                                applyProjectOutline( QVector< OutlineDocumentEntry > documents,
                                                             const QString& projectId, int truncated,
                                                             quint64 generation );

    // ── 용어집 ──
    /// 활성 프로젝트의 `.. glossary::` 를 다시 훑는다.
    /// force=false 면 같은 프로젝트에 대해 다시 돌지 않는다.
    void                                refreshGlossary( bool force );

    // ── 치환 ──
    /// 활성 프로젝트의 conf.py(rst_prolog/rst_epilog)와 문서의 `.. |name| ...` 를
    /// 다시 훑는다. force=false 면 같은 프로젝트에 대해 다시 돌지 않는다.
    void                                refreshSubstitutions( bool force );

    // ── 스크롤 동기화 ──
    // 에디터 -> 프리뷰, 프리뷰 -> 에디터 양방향. 어느 쪽이든 "기준 비율 위치에
    // 있는 줄" 을 상대편의 같은 비율 위치로 보낸다.
    void                                syncPreviewFromEditor();
    /// userInitiated 는 프리뷰를 직접 클릭한 이동. 왕복 방지 가드를 넘긴다.
    void                                syncEditorFromPreview( int sourceIndex, double line, double ratio,
                                                               bool userInitiated = false );
    [[nodiscard]] int                   sourceIndexForPath( const QString& path ) const;
    [[nodiscard]] QString               pathForSourceIndex( int sourceIndex ) const;

    ProjectRegistry*                    registry_ = nullptr;
    PythonEnvManager*                   pythonEnv_ = nullptr;
    PythonEnvResolver*                  envResolver_ = nullptr;
    QWebEngineView*                     previewView_ = nullptr;
    SphinxPreviewController*            previewController_ = nullptr;
    /// 내장 Markdown 렌더러. Sphinx 쪽과 나란한 형제다.
    MarkdownPreviewController*          markdownPreview_ = nullptr;
    VirtualProjectManager*              virtualProjects_ = nullptr;
    PreviewBridge*                      previewBridge_ = nullptr;
    DiagnosticsStore*                   diagnosticsStore_ = nullptr;
    LspServerPool*                      lspPool_ = nullptr;
    CompletionCoordinator*              completions_ = nullptr;
    GlossaryIndex*                      glossary_ = nullptr;
    /// 활성 프로젝트의 치환 목록. `|` 자동완성이 여기서 나온다.
    SubstitutionIndex*                  substitutions_ = nullptr;
    /// 워크스페이스 전역 경로 인덱스. 경로 완성이 처음 필요할 때 채워진다.
    PathIndex*                          pathIndex_ = nullptr;
    QString                             lspState_;
    /// 프리뷰의 원격 리소스 허용 상태. -1 은 아직 한 번도 적용하지 않은 것으로,
    /// 첫 적용에서 불필요한 리로드를 하지 않기 위해 구분한다.
    int                                 previewAllowRemote_ = -1;
    /// 마지막으로 적용한 수식 렌더러. 바뀌면 셸을 다시 읽어야 한다.
    QString                             previewMathRenderer_;
    /// 저장하지 않은 편집을 프리뷰에 반영할지 (설정 preview/applyUnsavedEdits).
    bool                                previewApplyUnsavedEdits_ = true;
    /// 직전 빌드의 재파싱 시간이 이 값을 넘는 문서는 반영에서 제외한다.
    /// 음수면 제한 없음. Breathe 문서처럼 재파싱이 수십 초인 경우를 막는다.
    int                                 previewUnsavedMaxReadMs_ = 2000;
    /// conf.py 정규식이 myst 라고 봤지만 빌더가 그 파일을 원본으로 읽지 않은
    /// 프로젝트. 세션 한정이고 rescanProjects() 에서 비운다.
    ///
    /// 프로젝트 단위로 기억하므로 그 2.5초는 프로젝트마다 한 번만 낸다 —
    /// 같은 프로젝트의 두 번째 `.md` 는 곧바로 내장 렌더러로 간다.
    QSet< QString >                     mystDeniedProjects_;
    QStringList                         previewSources_;      ///< data-mrr-src 인덱스 -> 원본 경로
    QStringList                         previewProcessedSources_;  ///< 이번 빌드가 다시 읽은 파일들
    /// 마지막으로 빌드를 **요청한** 문서. 탭을 옮겼는데 프리뷰가 따라오지
    /// 않은 상태를 알아채는 기준이다.
    QString                             previewRequestedPath_;
    /// 프리뷰에 실제로 **표시된** 문서.
    QString                             previewPrimaryPath_;
    /// 한쪽이 유발한 스크롤이 되돌아와 무한 왕복하는 것을 막는다.
    qint64                              suppressSyncUntilMs_ = 0;
    /// 에디터가 주도권을 쥔 구간. 이 동안 프리뷰의 스크롤 보고는 무시한다.
    qint64                              previewDrivenIgnoreUntilMs_ = 0;
    /// 가드에 걸려 버려진 에디터->프리뷰 동기화를 가드 해제 후 되살린다.
    QTimer*                             previewSyncRetry_ = nullptr;

    // ── 프리뷰 핫스왑 상태 ──
    // 같은 문서를 다시 빌드했고 <head> 가 그대로면 전체 리로드 대신 body 만
    // 갈아끼워 깜빡임을 없앤다.
    QString                             previewDocumentKey_;
    QString                             previewHeadSignature_;
    /// 지금 화면에 띄워 둔 HTML 의 크기와 mtime. 재빌드 결과가 이것과 같으면
    /// 파일 내용이 그대로라는 뜻이므로 다시 로드하지 않는다.
    qint64                              previewShownSize_ = -1;
    qint64                              previewShownMTimeMs_ = -1;
    /// 설정 preview/skipUnchangedBuild. 입력이 안 바뀌었으면 빌드를 건너뛴다.
    bool                                previewSkipUnchangedBuild_ = true;
    /// 설정 preview/stubDoxygenWhileTyping. 타이핑 중에는 doxygen 지시어를
    /// 자리표시자로 두어, 재파싱이 수십 초인 문서도 편집이 반영되게 한다.
    bool                                previewStubDoxygenWhileTyping_ = true;
    /// 늦게 도착한 이전 문서의 게이트 판정을 버리기 위한 세대 번호
    /// (outlineGeneration_ 과 같은 관용구).
    quint64                             previewGateGeneration_ = 0;
    QUrl                                previewUrl_;
    bool                                previewLoadedOk_ = false;
    /// load() 를 부르고 아직 loadFinished 를 못 받았는가.
    ///
    /// previewLoadedOk_ 로는 이것을 알 수 없다 — 로드 중과 "끝났지만 실패" 가
    /// 둘 다 false 라서, 그것만 보고 같은 URL 재요청을 막으면 실패한 로드를
    /// 다시 시도할 수 없게 된다.
    bool                                previewLoadInFlight_ = false;
    int                                 hotSwapToken_ = 0;
    QString                             pendingFullLoadPath_;
    /// 핫스왑이 실패했을 때 되돌아갈 URL. 캐시 무효화 쿼리까지 포함해야 한다.
    QUrl                                pendingFullLoadUrl_;
    /// 마지막으로 내보낸 프리뷰 상태 문구. 비어 있으면 "표시 없음".
    QString                             previewStatus_;

    // ── 개요 상태 ──
    QTimer*                             outlineDebounce_ = nullptr;
    QString                             projectOutlineProjectId_;
    /// 늦게 도착한 이전 프로젝트의 순회 결과를 버리기 위한 세대 번호.
    quint64                             outlineGeneration_ = 0;

    QHash< QTextView*, DocumentContext > documents_;
    QPointer< QTextView >               activeView_;
    QString                             activeProjectId_;
    bool                                shuttingDown_ = false;

    /// 세션 복원 중인가. 켜져 있으면 setActiveDocument() 가 반영을 미룬다.
    bool                                batchRestoring_ = false;
    /// 복원이 끝나면 활성화할 뷰.
    QPointer< QTextView >               batchPendingActive_;
};

}  // namespace mrst
