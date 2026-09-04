#pragma once

#include "core/solBaseView.hpp"
#include "core/solRestOutlineService.hpp"

#include "ui_mainWindow.h"

#include <QtGui>
#include <QtWidgets>
#include <QWebEngineView>
#include <QMainWindow>
#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QTabWidget>
#include <QToolBar>
#include <QLabel>
#include <QProgressBar>
#include <QCloseEvent>
#include <QAction>

class QActionGroup;
class QDragMoveEvent;
class QEvent;
class QImage;
class QMimeData;
class QPropertyAnimation;
class QPushButton;
class QTextView;
class QTimer;
class QVBoxLayout;

namespace ads {
class CDockManager;
class CDockWidget;
}

namespace mrst {
class ExternalChangeWatcher;
class FileTreeFilterProxy;
class PythonEnvManager;
class TabSwitcherPopup;
class UpdateService;
class WorkspaceController;
struct UpdateInfo;
struct WorkspaceSession;
}

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow( QWidget* parent = nullptr );
    ~MainWindow() override;

    void openFile( const QString& filePath );
    /// 명령줄 인자로 받은 경로들을 연다.
    /// 첫 인자가 폴더면 그것을 워크스페이스로 삼고 나머지를 파일로 연다.
    /// 첫 인자가 파일이면 그 상위 폴더가 워크스페이스가 된다.
    void openStartupPaths( const QStringList& paths );
    /// 명령줄 인자가 없을 때, 마지막 워크스페이스와 열려 있던 탭을 되살린다.
    void restoreLastSession();

    /// 기동 시 열 경로를 기억해 둔다. 비면 지난 세션을 복원한다.
    ///
    /// 왜 여기서 바로 열지 않는가: main() 의 show() 는 페인트를 **예약만** 하고
    /// 실제 첫 프레임은 exec() 가 이벤트 루프를 돌려야 나온다. 그래서 show() 와
    /// exec() 사이에서 탭을 열면 그 시간이 통째로 "창이 아무것도 안 그리는 시간"
    /// 이 된다. 실제 열기는 첫 페인트 뒤 advanceStartupPhase() 가 한다.
    void setStartupPaths( const QStringList& paths );

    QBaseView* currentView() const;

public slots:
    void                                onFileNew();
    void                                onFileOpen();
    void                                onWorkspaceOpen();
    void                                onFileSave();
    void                                onFileSaveAs();
    void                                onCopy();
    void                                onPaste();
    void                                onSettings();
    /// 도움말 → 정보. 아이콘/버전/저장소만 보여 주는 모달 대화상자를 띄운다.
    void                                onAbout();
    void                                onCloseTab( int index );
    void                                onTabChanged( int index );
    void                                onThemeToggle();
    /// F11. 프리뷰만 남기고 화면을 채운다. 다시 누르면 되돌린다.
    void                                togglePreviewFullScreen();

    void                                appendLog( const QString& text );

protected:
    bool                                eventFilter( QObject* watched, QEvent* event ) override;
    /// 첫 페인트가 오지 않는 경우(최소화 상태로 시작 등)의 안전망을 건다.
    void                                showEvent( QShowEvent* event ) override;
    /// QEvent::LanguageChange 를 여기서 받는다. 앱 전역 eventFilter 가 아니라
    /// 이쪽인 이유는 retranslateUi() 옆 주석에 적어 두었다.
    void                                changeEvent( QEvent* event ) override;
    void                                closeEvent( QCloseEvent* event ) override;
    void                                dragEnterEvent( QDragEnterEvent* event ) override;
    void                                dragMoveEvent( QDragMoveEvent* event ) override;
    void                                dropEvent( QDropEvent* event ) override;

private:
    struct ViewLoadingState
    {
        QString message;
        int value = 0;
        int maximum = 0;
    };

    /// 상태표시줄 **왼쪽**이 나타내는 작업 하나.
    ///
    /// 왼쪽은 "지금 무슨 일이 얼마나 되었는가" 한 자리다. 큰 `.rst` 를 열면 파일
    /// 읽기와 프리뷰가 함께 도는데, 그때 두 벌을 나란히 붙이면 좁은 줄에서
    /// 서로를 밀어낸다. 표로 모아 한 벌로 합쳐 낸다.
    struct StatusTask
    {
        QString message;
        /// 0..1000(천분율). 음수면 진행도를 알 수 없다(왕복 막대).
        ///
        /// 백분율이 아닌 이유: 프리뷰는 준비 · 빌드 읽기 · 빌드 쓰기 · HTML 로딩이
        /// 전체의 일부씩을 나눠 쓰므로 백분율로는 구간이 좁아 막대가 뚝뚝 끊긴다.
        int     permille    = -1;
        bool    cancellable = false;
        /// 취소 단추의 도구 설명. 무엇을 취소하는지 밝혀야 한다.
        QString cancelTip;
    };

    /// 작업의 우선순위. QMap 이 키로 정렬하므로 **선언 순서가 곧 순서**다.
    /// 파일 읽기가 앞이다 — 문서를 읽는 동안은 편집도 프리뷰도 할 수 없으니
    /// 사용자가 먼저 알아야 할 일이다.
    enum class StatusTaskId
    {
        FileLoad,
        Preview,
    };

    struct ViewTeardownOptions
    {
        ViewTeardownOptions( bool disconnectSignals = true,
                            bool blockViewSignals = false,
                            bool closeFile = true,
                            bool removeTab = true,
                            bool deleteLater = true )
            : disconnectSignals( disconnectSignals )
            , blockViewSignals( blockViewSignals )
            , closeFile( closeFile )
            , removeTab( removeTab )
            , deleteLater( deleteLater )
        {
        }

        bool disconnectSignals;
        bool blockViewSignals;
        bool closeFile;
        bool removeTab;
        bool deleteLater;
    };

    void createMenus();
    /// 메뉴의 표시 문자열을 (다시) 넣는다. createMenus() 가 마지막에 부르고,
    /// 언어가 바뀌면 다시 부른다. 메뉴 문자열은 여기에만 있다.
    void retranslateMenus();
    /// 언어가 바뀌었을 때 창 전체를 다시 칠한다. changeEvent() 가 부른다.
    void retranslateUi();
    void retranslateWorkspaceSearchTab();
    void retranslateOutlinePlaceholders();
    void retranslateUpdateBar();
    void                                setupCentralContainer();
    /// .ui 가 세로로 늘어놓은 pnl* 패널들을 Qt ADS 의 도크로 옮긴다.
    /// setupCentralContainer() 직후에 부른다 — 도크 매니저가 뷰어 도구모음
    /// 슬롯 아래에 들어가야 한다.
    void                                setupDockLayout();
    /// 도크 하나를 만들어 pnl* 위젯을 담는다.
    /// id 는 저장/복원 키다. 절대 번역하지 않는다 (setupDockLayout 주석 참고).
    ads::CDockWidget*                   makeDock( const char* id, const QString& title,
                                                  QWidget* content );
    /// 도크의 제목(= 탭 글자 = 사이드 탭 글자 = 보기 > 패널의 액션 글자)을
    /// 다시 넣는다.
    void                                retranslateDockTitles();
    /// 첫 실행(세션에 배치가 없을 때)의 패널 비율을 정한다. 창이 실제 크기를
    /// 가진 뒤에 불러야 한다 — 이유는 구현부 주석에 있다.
    void                                applyDefaultDockSizes();
    /// 요약 패널의 두 탭 중 어느 것을 앞에 둘지 활성 문서의 소속에 따라 정한다.
    /// WorkspaceController::activeDocumentResolved 에 걸려 있다.
    void                                applyDefaultOutlineTab( bool standalone );
    /// ADS 기본 스타일시트에서 다크 테마와 어긋나는 규칙을 덮는다.
    void                                applyDockStylesheetOverrides();
    /// 세션에 담긴 도크 배치(base64)를 되살린다. 비었거나 못 읽으면 기본 배치를
    /// 그대로 둔다.
    void                                restoreDockLayout( const QString& base64 );
    /// 중앙(편집기|프리뷰)을 뺀 모든 도크를 닫는다. 되돌리는 쌍은 없다 —
    /// 프리뷰 전체 화면에서 나올 때는 기억해 둔 배치를 restoreState() 로
    /// 통째로 되살리기 때문이다.
    void                                hideAllDockPanels();
    QString normalizeFilePath( const QString& filePath ) const;
    void                                applyThemeToView( QBaseView* view ) const;
    void                                applyCurrentTheme();
    void                                updateTitle();
    void                                updateTabDecoration( QBaseView* view );
    void                                updateViewerToolBar();
    void                                updateStatusBar();
    void                                connectViewStatusSignals( QBaseView* view );
    void                                updateSaveActionState();
    void                                updateCopyActionState();
    void                                updatePasteActionState();
    bool                                saveView( QBaseView* view, bool saveAs );
    void                                setViewLoadingState( QBaseView* view, bool active, const QString& message, int value, int maximum );
    /// 파일 열기/저장 진행 상태를 작업 표에 반영한다. 그리는 것은
    /// refreshStatusProgress() 가 한다.
    void                                refreshLoadingIndicator();
    /// 작업 표를 상태표시줄 왼쪽에 그린다.
    void                                refreshStatusProgress();
    /// 잠깐 보였다 사라지는 알림.
    ///
    /// **`QStatusBar::showMessage()` 를 쓰지 않는다.** 임시 메시지가 있는 동안 Qt 는
    /// `addWidget` 으로 붙인 위젯을 모두 숨기므로(qstatusbar.cpp: hideOrShow),
    /// 알림 하나가 몇 초 동안 진행막대를 지운다. `msec` 이 0 이면 다음 알림이나
    /// clearTransientStatus() 까지 남는다.
    void                                showTransientStatus( const QString& text, int msec = 0 );
    void                                clearTransientStatus();
    void                                advanceLoadingAnimation();
    void                                onCancelLoading();
    void                                addRecentFile( const QString& filePath );
    void                                addRecentWorkspace( const QString& folderPath );
    /// 파일과 워크스페이스 두 묶음을 한 메뉴에 다시 그린다.
    void                                updateRecentFilesMenu();
    /// 사라진 항목을 목록에서 빼고 메뉴와 설정을 갱신한다.
    void                                dropRecentFile( const QString& filePath );
    void                                dropRecentWorkspace( const QString& folderPath );
    /// 최근 목록에서 고른 항목을 연다. 그 사이 사라졌으면 알리고 목록에서 뺀다.
    void                                openRecentFile( const QString& filePath );
    void                                openRecentWorkspace( const QString& folderPath );
    void                                shutdownUi();

    /// 로그 한 건에 시각을 찍는다. `[ MM-dd HH:mm:ss.zzz ] ` 를 앞에 두고,
    /// 여러 줄이면 이어지는 줄을 같은 칸만큼 밀어 한 덩어리로 보이게 한다.
    [[nodiscard]] static QString        stampLogLine( const QString& text );
    int                                 addViewTab( QBaseView* view );
    void                                disconnectViewSignals( QBaseView* view );
    void                                removeViewTabWithoutSignals( QBaseView* view );
    void                                teardownView( QBaseView* view );
    void                                teardownView( QBaseView* view, const ViewTeardownOptions& options );
    void                                refreshCurrentViewUi();
    QBaseView*                          preferredLoadingView() const;
    void                                cancelLoadingView( QBaseView* view, bool showStatusMessage = true );
    QIcon                               tabIconForView( QBaseView* view ) const;
    QStringList                         droppedPathsFromMimeData( const QMimeData* mimeData ) const;
    bool                                hasDroppedPaths( const QMimeData* mimeData ) const;
    bool                                handleDropEvent( QDropEvent* event );
    void                                openDroppedPaths( const QStringList& paths );
    void                                openDroppedDirectory( const QString& dirPath );
    bool                                shouldConfirmBinaryTextOpen( const QString& filePath ) const;
    bool                                confirmOpenBinaryTextFile( const QString& filePath ) const;

    QBaseView*                          createViewForFile( const QString& filePath );
    void                                applyPersistedViewSettings( QBaseView* view );
    void                                applySettingsToAllViews();

    // ── 외부 파일 편집 인식 ──
    /// 감시 대상을 지금 열려 있는 탭 목록으로 맞춘다.
    ///
    /// 탭이 열리고 닫히고 "다른 이름으로 저장" 으로 경로가 바뀌는 것을 각각
    /// 추적하는 대신 매번 목록을 다시 만든다 — 탭은 열 손가락으로 셀 수 있고,
    /// 경로 하나를 놓치면 그 파일은 조용히 감시에서 빠진다.
    void                                refreshExternalWatchSet();
    void                                onExternalFileChanged( const QString& filePath );
    void                                onExternalFileVanished( const QString& filePath );
    /// 디스크 내용으로 다시 읽는다. 그 뷰가 이미 읽기/쓰기 중이면 끝난 뒤에
    /// 다시 시도한다 — 감시자는 이미 기준을 갱신했으므로 여기서 포기하면 그
    /// 변경은 다시 알려지지 않는다.
    void                                reloadViewFromDisk( QTextView* view, const QString& filePath );
    /// 물어보기로 결정된 파일을 줄 세운다. 자리를 비운 사이에 다섯 개가 바뀌면
    /// 모달 대화상자 다섯 개가 겹쳐 뜨는 것을 막는다.
    void                                queueExternalChangePrompt( const QString& filePath );
    void                                flushExternalChangePrompts();
    /// 그 경로를 열고 있는 텍스트 뷰. 없으면 nullptr.
    QTextView*                          textViewForPath( const QString& filePath ) const;
    /// 외부 변경 감시가 자기 저장을 남의 편집으로 오해하지 않게 뷰의 신호를 잇는다.
    void                                connectViewWatchSignals( QBaseView* view );

    // ── 탐색기 패널 ──
    /// 필터 줄 · 파일 조작 단추 · 정렬 · 컨텍스트 메뉴를 한자리에서 건다.
    void                                setupExplorerPanel();
    /// 도구 단추와 필터칸의 아이콘을 지금 팔레트로 다시 그린다.
    /// 아이콘은 그려서 만들므로 테마가 바뀌면 다시 만들어야 한다.
    void                                applyExplorerIcons();
    void                                retranslateExplorerPanel();
    /// 필터칸의 내용을 프록시에 넣고 펼침 상태를 맞춘다.
    void                                refreshExplorerFilter();

    // ── 필터가 걸린 동안 트리 훑기 ──
    //
    // QFileSystemModel 은 게을러서 읽기 전에는 자식이 모델에 없고, 필터는 모델에
    // 있는 것만 볼 수 있다. 그래서 필터가 켜지면 트리를 훑어 읽혀야 한다.
    //
    // **한 번에 하지 않는다.** 예전에는 한 호출이 트리 전체를 재귀로 돌며
    // 펼쳤고, 폴더 하나가 뒤늦게 읽힐 때마다 그 일을 처음부터 되풀이했다. 큰
    // 워크스페이스에서 그것이 UI 를 멈춰 세웠다. 지금은 일감을 큐에 담아
    // 한 차례에 kExplorerWalkChunk 개씩만 처리하고 이벤트 루프에 자리를 내준다.
    void                                beginExplorerFilterWalk();
    void                                stepExplorerFilterWalk();
    void                                stopExplorerFilterWalk();
    /// 디렉터리 하나를 일감에 넣는다 (원본 모델 인덱스). 예산이 다하면 무시한다.
    void                                queueExplorerDirectory( const QModelIndex& sourceIndex );

    /// 확장자 필터를 걷어내거나 되돌린다. 설정에 남는다.
    void                                setExplorerShowAllFiles( bool showAll );
    /// 필터칸이 포커스를 받으면 도구 단추 묶음을 접어 입력칸을 넓힌다.
    void                                setExplorerActionsCollapsed( bool collapsed );

    void                                onExplorerContextMenu( const QPoint& pos );
    /// 프록시 인덱스 -> 파일 정보. 유효하지 않으면 빈 QFileInfo.
    [[nodiscard]] QFileInfo             explorerFileInfo( const QModelIndex& proxyIndex ) const;
    [[nodiscard]] QFileInfo             explorerCurrentFileInfo() const;
    /// 새 항목을 만들 자리. 고른 것이 파일이면 그것이 든 폴더다.
    [[nodiscard]] QString               explorerTargetDirectory() const;
    void                                onExplorerNewFile();
    void                                onExplorerNewFolder();
    void                                onExplorerRename();
    void                                onExplorerDelete();
    /// 방금 만들거나 이름을 바꾼 항목을 트리에서 골라 보여 준다.
    void                                selectExplorerPath( const QString& path );
    /// 지금 펼쳐져 있는 폴더들의 절대 경로.
    [[nodiscard]] QStringList           expandedExplorerPaths() const;
    void                                restoreExplorerExpansion( const QStringList& paths );
    /// 이 디렉터리가 **실제** Sphinx 프로젝트의 루트면 그 projectId.
    /// 가상 프로젝트와 그냥 폴더는 빈 문자열이다.
    [[nodiscard]] QString               projectIdForDirectory( const QString& dirPath ) const;
    void                                onExplorerBuild( const QString& projectId,
                                                         const QString& projectRoot );
    /// 파일 관리자에서 그 경로를 보여 준다 (파일이면 선택된 채로).
    void                                revealInFileManager( const QString& path );

    /// 지금 탭의 편집기에 키보드 포커스를 준다.
    ///
    /// 시작할 때 이것이 없으면 Qt 가 **탭 순서의 첫 위젯**에 포커스를 준다.
    /// 좌측 패널에 필터 입력칸이 생기면서 그 자리가 필터로 바뀌어, 창이 뜨자마자
    /// 친 글자가 문서가 아니라 필터로 들어갔다.
    void                                focusActiveEditor();

    // ── 요약 탭 필터 ──
    /// 트리 항목을 문구로 거른다. 자손이 걸리면 조상도 남기고 펼친다.
    /// 남은 것이 하나도 없으면 false.
    bool                                applyOutlineFilter( QTreeWidget* tree, const QString& text );
    void                                refreshOutlineFilters();

    Ui::MainWindow                      Ui;
    QFileSystemModel*                   treLeftFolderTreeModel_ = nullptr;
    /// 탐색기 트리의 필터 · 정렬. treLeftFolderTreeModel_ 위에 얹는다.
    mrst::FileTreeFilterProxy*          explorerProxy_ = nullptr;
    /// 한 글자마다 트리를 다시 펼치지 않도록.
    QTimer*                             explorerFilterDebounce_ = nullptr;
    /// 필터를 걸기 **직전**에 펼쳐져 있던 폴더들. 필터를 지우면 이대로 되돌린다.
    QStringList                         explorerExpandedBeforeFilter_;
    /// 아직 훑지 않은 디렉터리들 (원본 모델 인덱스). 앞에서 꺼내 너비 우선으로
    /// 돈다 — 얕은 곳이 먼저 보여야 사용자가 결과를 일찍 본다.
    QList< QPersistentModelIndex >      explorerWalkQueue_;
    /// explorerWalkQueue_ 를 조금씩 비운다. 간격 0 — 밀린 이벤트 뒤에 깨어난다.
    QTimer*                             explorerWalkTimer_ = nullptr;
    /// 이번 필터에서 큐에 더 넣을 수 있는 디렉터리 수. 0 이면 거기서 멈춘다.
    int                                 explorerWalkBudget_ = 0;
    /// 도구 단추 묶음을 접고 펴는 애니메이션. 처음 쓸 때 만든다.
    QPropertyAnimation*                 explorerActionsAnimation_ = nullptr;

    QWidget*                            m_centralContainer = nullptr;
    /// .ui 의 centralwidget. setupCentralContainer() 가 떼어내 들고 있다가
    /// setupDockLayout() 이 내용을 도크로 옮긴 뒤 지운다. 그 뒤로는 nullptr 다.
    QWidget*                            m_uiCentralShell = nullptr;
    QWidget*                            m_viewerToolBarHost = nullptr;
    QVBoxLayout*                        m_viewerToolBarLayout = nullptr;

    // ── 도킹 (Qt ADS) ──
    /// 좌측·하단 패널의 배치를 쥐고 있다. m_centralContainer 안에 있다.
    ads::CDockManager*                  dockManager_ = nullptr;
    /// 편집기 | 프리뷰. 닫거나 떼어낼 수 없는 중앙 도크다.
    ads::CDockWidget*                   dockEditor_ = nullptr;
    ads::CDockWidget*                   dockExplorer_ = nullptr;
    ads::CDockWidget*                   dockOutlineDocument_ = nullptr;
    ads::CDockWidget*                   dockOutlineProject_ = nullptr;
    ads::CDockWidget*                   dockDiagnostics_ = nullptr;
    ads::CDockWidget*                   dockLog_ = nullptr;
    /// setupWorkspaceSearchTab() 이 만든다 (내용이 코드로 조립되기 때문).
    ads::CDockWidget*                   dockSearch_ = nullptr;
    /// 보기 > 패널. 닫은 패널을 되살리는 유일한 수단이다.
    QMenu*                              dockPanelsMenu_ = nullptr;
    QTabWidget*                         m_tabWidget = nullptr;
    QToolBar*                           m_mainToolBar = nullptr;
    QPointer<QToolBar>                  m_viewerToolBar = nullptr;   // 뷰어별 도구모음
    QPointer<QToolBar>                  m_viewerAuxToolBar = nullptr; // 뷰어별 2줄 보조 도구모음
    // ── 상태표시줄 왼쪽 (진행 상황) ──
    QLabel*                             statusMessageLabel_ = nullptr;
    QProgressBar*                       statusProgressBar_ = nullptr;
    QPushButton*                        statusCancelButton_ = nullptr;
    /// 지금 도는 작업들. 비면 왼쪽에는 임시 알림만 남는다.
    QMap< StatusTaskId, StatusTask >    statusTasks_;
    /// 임시 알림 문구. 작업이 도는 동안은 가려진다.
    QString                             transientStatus_;
    QTimer*                             transientStatusTimer_ = nullptr;

    QLabel*                             m_statusLabel = nullptr;   // 상태 표시줄 정보(오른쪽)
    QMenu*                              m_recentMenu = nullptr;   // 최근 파일 / 워크스페이스 메뉴
    QAction*                            m_saveAction = nullptr;
    QAction*                            m_saveAsAction = nullptr;
    QAction*                            m_captureAction = nullptr;
    QAction*                            m_copyAction = nullptr;
    QAction*                            m_pasteAction = nullptr;
    QHash<QBaseView*, ViewLoadingState> m_activeViewLoads;
    QTimer*                             m_loadingAnimationTimer = nullptr;
    int                                 m_loadingAnimationFrame = 0;
    QStringList                         m_recentFiles;
    /// 최근에 연 워크스페이스 폴더. 파일 목록과 같은 메뉴에 아래쪽으로 놓인다.
    QStringList                         m_recentWorkspaces;
    bool                                m_shuttingDown = false;

    mrst::ExternalChangeWatcher*        externalWatcher_ = nullptr;
    /// "사용자에게 묻기" 대기열. 창이 활성일 때만 비운다 — 다른 앱에서 일하는
    /// 사람 앞에 우리 모달을 들이밀지 않는다.
    QStringList                         externalPromptQueue_;
    bool                                externalPromptActive_ = false;

    // ── 단계적 기동 ──
    // 생성자는 껍데기만 만들고(P0), 무거운 것은 첫 페인트 뒤로 미룬다(P1).
    // 그러지 않으면 Chromium 부팅과 탭 복원이 첫 프레임 앞을 막는다.
    enum class StartupPhase
    {
        Shell,      ///< 생성자가 끝난 상태. 창은 떴지만 프리뷰도 탭도 없다.
        Ready,      ///< 프리뷰가 붙고 세션이 복원된 상태.
    };
    StartupPhase                        startupPhase_ = StartupPhase::Shell;

    // ── UI 정체 감시자 (MRST_PHASE_TRACE 가 있을 때만 존재한다) ──
    //
    // PhaseSpan 은 "우리가 의심한 구간" 만 잰다. 그것으로는 **의심하지 않은 곳**이
    // 이벤트 루프를 굶기는 경우를 못 잡는다 — 이 앱에서 응답없음의 후보 절반은
    // Chromium 합성이나 Scintilla 유휴 재배치처럼 우리 코드가 아닌 곳에 있다.
    //
    // 그래서 GUI 스레드에서 50 ms 타이머를 돌리고 **실제 간격**을 잰다. 간격이
    // 크게 벌어진 만큼이 사용자가 느끼는 멈칫이다. 바깥에서
    // SendMessageTimeout 으로 재는 방법은 창 핸들이 어긋나면 조용히 0 을 내므로
    // 신뢰할 수 없었다.
    /// 전체 화면 복귀의 배치 복원(singleShot(0))이 아직 돌지 않았는가.
    ///
    /// 그 창 안에서 F11 이 다시 들어오면 saveState() 가 "모든 도크가 닫힌" 배치를
    /// 정상 배치로 기억한다. 이 표시가 그것을 막는다.
    bool                                fullScreenRestorePending_ = false;

    /// 상태바 갱신을 이벤트 루프 회전당 1회로 접는다. scheduleStatusBarRefresh()
    /// 가 세우고 그 타이머가 지운다.
    bool                                statusBarRefreshPending_ = false;
    void                                scheduleStatusBarRefresh();

    /// 진단 표 재구축도 같은 방식으로 접는다. refreshDiagnosticsTable() 은
    /// 행마다 QTableWidgetItem 다섯 개와 QFileInfo 하나를 새로 만들므로,
    /// 한 빌드가 낸 여러 changed() 를 그대로 받으면 그 값을 배로 치른다.
    bool                                diagnosticsTableRefreshPending_ = false;
    /// Markdown 활성 중에는 진단 표를 숨긴다. reST 탭으로 돌아올 때 저장소의
    /// 진단을 한 번만 복원하기 위한 상태다.
    bool                                documentPanelsHiddenForMarkdown_ = false;
    void                                scheduleDiagnosticsTableRefresh();

    QTimer*                             stallWatchdog_ = nullptr;
    QElapsedTimer                       stallClock_;
    qint64                              stallLastTickNs_ = 0;
    void                                startStallWatchdog();
    /// 첫 페인트를 이미 봤는가. eventFilter 의 일회성 분기를 단락시킨다.
    bool                                firstPaintSeen_ = false;
    /// QWebEnginePage 를 우리가 명시적으로 만들었는가.
    /// QWebEngineView::page() 는 **없으면 만들어 버리므로** 존재 여부를 물어보는
    /// 수단이 없다. 그래서 직접 들고 있어야 한다.
    bool                                previewInitialised_ = false;
    /// 세션이 프리뷰 스플리터 배치를 복원했는가. 복원했으면 기본 배분으로 덮지 않는다.
    bool                                previewSplitFromSession_ = false;
    /// 세션이 도크 배치를 복원했는가. 위와 같은 이유로 둔다 — 사용자가 옮겨 둔
    /// 패널을 기본 비율로 되돌리면 그 조작이 매 실행마다 사라진다.
    bool                                dockLayoutFromSession_ = false;
    /// 요약 패널의 "활성 문서" 탭을 **우리가** 앞으로 냈는가.
    /// 되돌릴 대상을 우리 조작으로 한정하려고 둔다 — 사용자가 직접 고른 탭은
    /// 문서가 바뀌어도 그대로 둔다.
    bool                                outlineTabAutoSwitched_ = false;
    /// setStartupPaths() 로 받아 둔 명령줄 경로. 비면 지난 세션을 복원한다.
    QStringList                         startupPaths_;

    /// 다음 기동 단계로 넘어간다. **멱등**이다 — 첫 페인트와 안전망 타이머가
    /// 둘 다 부를 수 있고, 사용자 조작(파일 열기/드롭)이 앞당길 수도 있다.
    void                                advanceStartupPhase();
    /// QWebEnginePage 를 만들고 컨트롤러에 붙인다. 여기서 Chromium 이 뜬다.
    void                                initialisePreview();
    /// 프리뷰가 0 폭으로 시작하는 것을 막는다. 세션이 정한 배치는 덮지 않는다.
    void                                ensureVisiblePreviewSplit();
    /// hot-exit 스냅샷 복원. 생성자에서 하면 첫 프레임 앞을 막는다.
    void                                restoreHotExitSnapshots();

    // ── 프리뷰 전체 화면 (F11) ──
    /// 전체 화면에 들어가기 전의 화면 배치.
    ///
    /// **프리뷰를 별도 창으로 떼어내지 않는다.** QWebEngineView 는 자기 합성
    /// 표면을 갖고 있어 부모를 갈아 끼우면 페이지가 다시 붙는 동안 화면이 비고,
    /// 무엇보다 탭 목록(Ctrl+Tab)이 어느 창에 떠야 하는지가 애매해진다. 그래서
    /// **같은 창에서 프리뷰 말고 전부 감춘다** — 탭 위젯은 숨겨진 채로 살아
    /// 있으므로 탭 전환과 그에 딸린 프리뷰 빌드가 평소 경로대로 돈다.
    struct PreviewFullScreenState
    {
        bool                            active = false;
        Qt::WindowStates                windowStates = Qt::WindowNoState;
        /// 들어가기 직전의 창 크기·위치. 세션에 저장할 때 이 값을 쓴다 —
        /// 전체 화면 중에 saveGeometry() 를 찍으면 그 플래그까지 담기고,
        /// 다음 실행이 메뉴도 편집기도 없는 전체 화면으로 뜬다.
        QByteArray                      windowGeometry;
        QList< int >                    previewSplitSizes;
        /// 좌측·하단 도크의 배치 전체. bool 세 개로는 모자란다 — 어느 패널을
        /// 가장자리에 핀 고정해 두었는지, 하단에서 어느 탭을 보고 있었는지,
        /// 떼어내 띄운 창이 있었는지까지 여기 들어 있다. 나올 때 그대로
        /// 되돌려야 "들어가기 전 상태" 가 지켜진다.
        QByteArray                      dockState;
        bool                            editorVisible = true;
        bool                            menuBarVisible = true;
        bool                            statusBarVisible = true;
        /// 전체 화면 동안 창에 함께 걸어 둔 메뉴 액션들.
        ///
        /// 메뉴 바를 감추면 그 안의 액션 단축키가 **함께 죽는다**. Qt 는 메뉴
        /// 항목의 단축키를 판정할 때 그 메뉴를 물고 있는 위젯(여기서는 메뉴
        /// 바)이 보이는지부터 보기 때문이다. 그래서 같은 QAction 을 창에도
        /// 걸어 둔다 — 액션 하나를 두 위젯에 거는 것은 모호한 단축키가 되지
        /// 않는다(연결된 위젯 중 **하나라도** 맞으면 발동한다).
        QList< QPointer< QAction > >    borrowedActions;
    };
    PreviewFullScreenState              previewFullScreen_;
    QAction*                            previewFullScreenAction_ = nullptr;
    /// 전체 화면에서만 살아 있는 Esc. 평소에 켜 두면 찾기 상자의 Esc 를 훔친다.
    QAction*                            previewExitFullScreenAction_ = nullptr;

    void                                setPreviewFullScreen( bool enabled );

    // ── 탭 목록 (Ctrl+Tab) ──
    /// 최근 사용 순서. 앞이 가장 최근이다.
    ///
    /// Visual Studio 의 Ctrl+Tab 목록이 이 순서로 나오고, 그래서 한 번 누르고
    /// 떼면 **직전 문서**로 간다 — 두 문서를 번갈아 보는 동작이 한 손동작이 된다.
    /// 탭 순서로 나열하면 그 동작이 사라진다.
    QList< QPointer< QBaseView > >      tabMruOrder_;
    QPointer< mrst::TabSwitcherPopup >  tabSwitcher_;

    /// 이 뷰를 최근 사용 목록 맨 앞으로 올린다. 죽은 항목도 여기서 걷어낸다.
    void                                noteTabActivated( QBaseView* view );
    /// 탭 목록 팝업을 띄운다. forward=false 면 가장 오래된 항목부터 강조한다.
    void                                showTabSwitcher( bool forward );

private:

    ///////////////////////////////////////////////////////////////////////////
    /// Esbonio / Sphinx
    ///
    /// MainWindow 는 UI 셸만 담당하고, 프로젝트 스캔 / LSP / 프리뷰 / 진단은
    /// 전부 WorkspaceController 가 소유한다. 여기서는 탭 수명주기 이벤트를
    /// 컨트롤러로 전달하기만 한다.

    void                                setWorkspace( const QString& Folder );
    void                                refreshProjectList();
    [[nodiscard]] QTextView*            textViewOf( QBaseView* view ) const;

    void                                setupPythonEnvironment();
    void                                updateEnvStatusChip();
    void                                confirmRepairPythonEnvironment();
    void                                setupDiagnosticsTable();
    void                                refreshDiagnosticsTable();
    /// 문서/프로젝트 개요 트리. 컨트롤러가 주는 항목을 그리고 클릭 시 이동한다.
    void                                setupOutlineTrees();
    /// 캐시한 개요를 지금 설정의 깊이로 다시 그린다. 컨트롤러에 다시 묻지
    /// 않는다 — 프로젝트 개요는 문서 수백 개를 디스크에서 읽는 작업이라,
    /// 설정 대화상자에서 스핀박스를 한 칸 돌릴 때 할 일이 아니다.
    void                                redrawDocumentOutlineTree();
    void                                redrawProjectOutlineTree();
    /// 설정에서 개요 깊이를 다시 읽는다. 값이 달라졌으면 두 트리를 다시 그린다.
    void                                reloadOutlineDepth();
    void                                onOutlineItemActivated( QTreeWidgetItem* item, int column );
    /// 누락된 Sphinx 확장/테마를 알리는 비모달 바.
    /// 프리뷰 빌드는 디바운스 타이머로 발화하므로 모달을 띄우면 쓸 수 없게 된다.
    void                                setupMissingDependencyBar();
    void                                showMissingDependencies( const QStringList& distributions );

    // ── 자동 업데이트 ──
    /// 새 버전을 알리는 비모달 바. missingDepBar_ 와 생김새는 같지만 자리가
    /// 다르다 — 업데이트는 프리뷰가 아니라 앱 전역의 사건이라 창 폭 전체를 쓴다.
    void                                setupUpdateBar();
    void                                setupUpdateService();
    void                                showUpdateAvailable( const mrst::UpdateInfo& info );
    void                                showUpdateReady( const QString& version );
    /// 설치를 확인받고 앱을 닫는다. 실제 교체는 종료 경로 끝에서 시작된다.
    void                                confirmInstallNow();

    // ── 세션 영속성 ──
    /// 지금 워크스페이스의 열린 탭/캐럿/스플리터를 <root>/.multiroot/workspace.json 에 쓴다.
    void                                saveWorkspaceSessionNow();

    /// 창 크기·위치·패널 배치를 워크스페이스마다 기억하는가 (설정
    /// `window/restoreLayout`, 기본 켜짐).
    [[nodiscard]] static bool           layoutRestoreEnabled();
    /// 지금 창 상태를 base64 로 만든다. 전체 화면 중이면 들어가기 직전 값을 쓴다.
    [[nodiscard]] QString               currentWindowGeometry() const;
    /// 세션에 담긴 창 상태(base64)를 되살린다. 비었거나 못 읽으면 아무것도 하지
    /// 않는다. **show() 보다 먼저** 불러야 창이 한 번 떴다가 크기가 바뀌지 않는다.
    void                                restoreWindowGeometry( const QString& base64 );
    /// 마지막 워크스페이스의 세션에서 창 상태만 꺼내 적용한다. 생성자가 부른다 —
    /// 탭 복원(restoreLastSession)은 첫 페인트 뒤라서 그때는 이미 늦다.
    void                                restoreWindowGeometryForLastWorkspace();
    /// 세션에 담긴 화면 배치(도크 · 편집기|프리뷰 스플리터 · 창)를 적용한다.
    /// 탭은 건드리지 않는다. 배치 기억 설정이 꺼져 있으면 아무것도 하지 않는다.
    void                                applySessionLayout( const mrst::WorkspaceSession& session );
    /// 창 크기를 이미 세션에서 되살렸는가. 두 번 적용해 사용자가 그 사이 옮긴
    /// 창을 되돌리지 않기 위한 표시다.
    bool                                windowGeometryRestored_ = false;

    // ── 워크스페이스 검색 ──
    void                                setupWorkspaceSearchTab();
    void                                runWorkspaceSearch();
    void                                runWorkspaceReplacePreview();
    void                                applyWorkspaceReplace();

    mrst::WorkspaceController*          controller_ = nullptr;
    mrst::PythonEnvManager*             pythonEnv_ = nullptr;
    QPushButton*                        envStatusLabel_ = nullptr;   // 상태 표시줄 환경 칩
    struct DamagedPythonStatus
    {
        QString                         projectId;
        QString                         environmentPath;
        QString                         reason;
    };
    QMap< QString, DamagedPythonStatus > damagedPythonEnvironments_;
    QString                             preferredDamagedPythonProjectKey_;
    QString                             repairingPythonProjectKey_;
    int                                 pythonRepairPercent_ = -1;
    QString                             pythonRepairPhase_;
    /// 상태 표시줄 프리뷰 칩. 빌드부터 HTML 로드 완료까지가 한 구간이라,
    /// 그동안 아무 표시가 없으면 사용자에게는 고장으로 보인다.
    QWidget*                            missingDepBar_ = nullptr;
    QLabel*                             missingDepLabel_ = nullptr;
    QStringList                         missingDepPending_;
    QStringList                         missingDepDismissed_;        // 이번 세션에 거절한 것

    mrst::UpdateService*                updateService_ = nullptr;
    QWidget*                            updateBar_ = nullptr;
    QLabel*                             updateLabel_ = nullptr;
    QPushButton*                        updateActionButton_ = nullptr;   // [내려받기] / [지금 재시작]
    QPushButton*                        updateNotesButton_ = nullptr;
    QPushButton*                        updateSkipButton_ = nullptr;
    QPushButton*                        updateLaterButton_ = nullptr;
    QLabel*                             updateStatusLabel_ = nullptr;    // 상태 표시줄 업데이트 칩
    QString                             updateDismissedVersion_;         // "나중에" 는 이번 세션만
    /// 종료 경로 끝에서 업데이터를 띄울지. closeEvent 가 저장 확인에서 되돌아가면
    /// 반드시 다시 false 로 만든다 — 아니면 다음 종료 때 설치가 시작된다.
    bool                                pendingInstall_ = false;


    // ── 개요 트리 ──
    /// 트리에 나타낼 섹션 단계. 0 은 제한하지 않음
    /// (설정의 preview/outlineMaxDepth, 기본 3단계).
    int                                 outlineMaxDepth_ = 3;
    /// 마지막으로 컨트롤러가 준 개요. 깊이 설정이 바뀌면 이것으로 다시 그린다.
    /// 비어 있으면 트리가 안내 문구를 보여 주는 중이라 손대지 않는다.
    QVector< mrst::OutlineSymbol >      outlineDocumentSymbols_;
    QVector< mrst::OutlineDocumentEntry > outlineProjectDocuments_;
    int                                 outlineProjectTruncated_ = 0;

    QString                             workspaceRoot_;

    // 검색 탭 위젯들. .ui 를 건드리지 않고 코드로 만든다.
    /// 검색 탭의 페이지 위젯. 탭 제목을 다시 칠할 때 indexOf() 에 쓴다
    /// (런타임에 붙인 탭이라 uic 의 retranslateUi() 가 건드리지 않는다).
    QWidget*                            searchTabPage_ = nullptr;
    QLineEdit*                          searchQueryEdit_ = nullptr;
    QLineEdit*                          searchReplaceEdit_ = nullptr;
    QCheckBox*                          searchCaseBox_ = nullptr;
    QCheckBox*                          searchWordBox_ = nullptr;
    QCheckBox*                          searchRegexBox_ = nullptr;
    QTreeWidget*                        searchResultTree_ = nullptr;
    QLabel*                             searchStatusLabel_ = nullptr;
    QPushButton*                        searchApplyButton_ = nullptr;
    /// 미리보기에서 확인한 대상. [적용]은 이 목록에만 쓴다.
    QStringList                         pendingReplacePaths_;
};
