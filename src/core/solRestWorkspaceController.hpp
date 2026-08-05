#pragma once

#include "solSphinxDiagnostics.hpp"
#include "solSphinxScanner.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

class QTextView;
class QWebEngineView;

namespace mrst {

class PreviewBridge;
class ProjectRegistry;
class PythonEnvManager;
class SphinxPreviewController;
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

    /// 활성 문서의 프리뷰를 다시 빌드한다.
    /// immediate=false 면 디바운스(편집 중), true 면 즉시(저장/탭 전환).
    void                                requestPreviewBuild( bool immediate = false );

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

private:
    [[nodiscard]] DocumentContext*      contextFor( QTextView* view );
    void                                resolveProject( DocumentContext& context );
    void                                logProjectList();
    void                                onPreviewFinished( const PreviewBuildResult& result );
    [[nodiscard]] QString               writeShadowCopy( QTextView* view, const QString& path ) const;

    // ── 스크롤 동기화 ──
    // 에디터 -> 프리뷰, 프리뷰 -> 에디터 양방향. 어느 쪽이든 "기준 비율 위치에
    // 있는 줄" 을 상대편의 같은 비율 위치로 보낸다.
    void                                syncPreviewFromEditor();
    void                                syncEditorFromPreview( int sourceIndex, double line, double ratio );
    [[nodiscard]] int                   sourceIndexForPath( const QString& path ) const;
    [[nodiscard]] QString               pathForSourceIndex( int sourceIndex ) const;

    ProjectRegistry*                    registry_ = nullptr;
    PythonEnvManager*                   pythonEnv_ = nullptr;
    QWebEngineView*                     previewView_ = nullptr;
    SphinxPreviewController*            previewController_ = nullptr;
    PreviewBridge*                      previewBridge_ = nullptr;
    QStringList                         previewSources_;      ///< data-mrr-src 인덱스 -> 원본 경로
    /// 한쪽이 유발한 스크롤이 되돌아와 무한 왕복하는 것을 막는다.
    qint64                              suppressSyncUntilMs_ = 0;

    QHash< QTextView*, DocumentContext > documents_;
    QPointer< QTextView >               activeView_;
    QString                             activeProjectId_;
    bool                                shuttingDown_ = false;
};

}  // namespace mrst
