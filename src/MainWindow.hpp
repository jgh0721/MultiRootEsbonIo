#pragma once

#include "core/solBaseView.hpp"
#include "core/solRestOutlineService.hpp"

#include "ui_mainWindow.h"

#include <QtGui>
#include <QtWidgets>
#include <QWebEngineView>
#include <QMainWindow>
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
class QPushButton;
class QTextView;
class QTimer;
class QVBoxLayout;

namespace mrst {
class ExternalChangeWatcher;
class PythonEnvManager;
class TabSwitcherPopup;
class UpdateService;
class WorkspaceController;
struct UpdateInfo;
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
    void                                refreshLoadingIndicator();
    void                                advanceLoadingAnimation();
    void                                onCancelLoading();
    void                                addRecentFile( const QString& filePath );
    void                                updateRecentFilesMenu();
    void                                shutdownUi();
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

    Ui::MainWindow                      Ui;
    QFileSystemModel*                   treLeftFolderTreeModel_ = nullptr;

    QWidget*                            m_centralContainer = nullptr;
    QWidget*                            m_viewerToolBarHost = nullptr;
    QVBoxLayout*                        m_viewerToolBarLayout = nullptr;
    QTabWidget*                         m_tabWidget = nullptr;
    QToolBar*                           m_mainToolBar = nullptr;
    QPointer<QToolBar>                  m_viewerToolBar = nullptr;   // 뷰어별 도구모음
    QPointer<QToolBar>                  m_viewerAuxToolBar = nullptr; // 뷰어별 2줄 보조 도구모음
    QLabel*                             m_loadingLabel = nullptr;
    QProgressBar*                       m_loadingProgressBar = nullptr;
    QPushButton*                        m_loadingCancelButton = nullptr;
    QLabel*                             m_statusLabel = nullptr;   // 상태 표시줄 정보
    QMenu*                              m_recentMenu = nullptr;   // 최근 파일 메뉴
    QAction*                            m_saveAction = nullptr;
    QAction*                            m_saveAsAction = nullptr;
    QAction*                            m_captureAction = nullptr;
    QAction*                            m_copyAction = nullptr;
    QAction*                            m_pasteAction = nullptr;
    QHash<QBaseView*, ViewLoadingState> m_activeViewLoads;
    QTimer*                             m_loadingAnimationTimer = nullptr;
    int                                 m_loadingAnimationFrame = 0;
    QStringList                         m_recentFiles;
    static constexpr int                MaxRecentFiles = 10;
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
    /// 첫 페인트를 이미 봤는가. eventFilter 의 일회성 분기를 단락시킨다.
    bool                                firstPaintSeen_ = false;
    /// QWebEnginePage 를 우리가 명시적으로 만들었는가.
    /// QWebEngineView::page() 는 **없으면 만들어 버리므로** 존재 여부를 물어보는
    /// 수단이 없다. 그래서 직접 들고 있어야 한다.
    bool                                previewInitialised_ = false;
    /// 세션이 프리뷰 스플리터 배치를 복원했는가. 복원했으면 기본 배분으로 덮지 않는다.
    bool                                previewSplitFromSession_ = false;
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
        QList< int >                    previewSplitSizes;
        QList< int >                    contentSplitSizes;
        QList< int >                    sideSplitSizes;
        bool                            sidePanelVisible = true;
        bool                            bottomVisible = true;
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

    // ── 워크스페이스 검색 ──
    void                                setupWorkspaceSearchTab();
    void                                runWorkspaceSearch();
    void                                runWorkspaceReplacePreview();
    void                                applyWorkspaceReplace();

    mrst::WorkspaceController*          controller_ = nullptr;
    mrst::PythonEnvManager*             pythonEnv_ = nullptr;
    QLabel*                             envStatusLabel_ = nullptr;   // 상태 표시줄 환경 칩
    /// 상태 표시줄 프리뷰 칩. 빌드부터 HTML 로드 완료까지가 한 구간이라,
    /// 그동안 아무 표시가 없으면 사용자에게는 고장으로 보인다.
    QLabel*                             previewStatusLabel_ = nullptr;
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
