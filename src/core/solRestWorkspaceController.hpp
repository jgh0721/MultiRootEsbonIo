#pragma once

#include "solRestOutlineService.hpp"
#include "solSphinxDiagnostics.hpp"
#include "solSphinxScanner.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
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
class PreviewBridge;
class ProjectRegistry;
class PythonEnvManager;
class PythonEnvResolver;
struct ResolvedPythonEnv;
class SphinxPreviewController;
class VirtualProjectManager;
struct PreviewBuildResult;

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

    // ── 문서 수명주기 (MainWindow 의 탭 배관에서 호출) ──
    void                                attachDocument( QTextView* view );
    void                                detachDocument( QTextView* view );
    void                                setActiveDocument( QTextView* view );
    void                                notifyDocumentSaved( QTextView* view );

    [[nodiscard]] QString               activeProjectId() const;
    [[nodiscard]] DiagnosticsStore*     diagnostics() const;

    /// 활성 문서의 프리뷰를 다시 빌드한다.
    /// immediate=false 면 디바운스(편집 중), true 면 즉시(저장/탭 전환).
    void                                requestPreviewBuild( bool immediate = false );

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
    void                                logProjectList();
    void                                onPreviewFinished( const PreviewBuildResult& result );
    [[nodiscard]] QString               writeShadowCopy( QTextView* view, const QString& path ) const;
    void                                showPreviewHtml( const QString& htmlPath, const QString& documentKey );
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
    VirtualProjectManager*              virtualProjects_ = nullptr;
    PreviewBridge*                      previewBridge_ = nullptr;
    DiagnosticsStore*                   diagnosticsStore_ = nullptr;
    LspServerPool*                      lspPool_ = nullptr;
    CompletionCoordinator*              completions_ = nullptr;
    GlossaryIndex*                      glossary_ = nullptr;
    QString                             lspState_;
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
    QUrl                                previewUrl_;
    bool                                previewLoadedOk_ = false;
    int                                 hotSwapToken_ = 0;
    QString                             pendingFullLoadPath_;

    // ── 개요 상태 ──
    QTimer*                             outlineDebounce_ = nullptr;
    QString                             projectOutlineProjectId_;
    /// 늦게 도착한 이전 프로젝트의 순회 결과를 버리기 위한 세대 번호.
    quint64                             outlineGeneration_ = 0;

    QHash< QTextView*, DocumentContext > documents_;
    QPointer< QTextView >               activeView_;
    QString                             activeProjectId_;
    bool                                shuttingDown_ = false;
};

}  // namespace mrst
