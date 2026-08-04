#pragma once

#include "solSphinxScanner.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class QTextView;
class QWebEngineView;

namespace mrst {

class ProjectRegistry;
class SphinxPreviewController;

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

signals:
    void                                logMessage( const QString& text );
    void                                projectsChanged( int count );
    void                                activeProjectChanged( const QString& projectId, bool isVirtual );
    void                                navigateRequested( const QString& path, int line, int column );

private:
    [[nodiscard]] DocumentContext*      contextFor( QTextView* view );
    void                                resolveProject( DocumentContext& context );
    void                                logProjectList();

    ProjectRegistry*                    registry_ = nullptr;
    QWebEngineView*                     previewView_ = nullptr;
    SphinxPreviewController*            previewController_ = nullptr;

    QHash< QTextView*, DocumentContext > documents_;
    QPointer< QTextView >               activeView_;
    QString                             activeProjectId_;
    bool                                shuttingDown_ = false;
};

}  // namespace mrst
