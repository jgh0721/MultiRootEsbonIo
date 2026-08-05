#pragma once

#include "core/solBaseView.hpp"

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
class PythonEnvManager;
class WorkspaceController;
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
    QBaseView* currentView() const;

public slots:
    void                                onFileNew();
    void                                onFileOpen();
    void                                onWorkspaceOpen();
    void                                onFileSave();
    void                                onFileSaveAs();
    void                                onCopy();
    void                                onPaste();
    void                                onResetClipboardImagePastePrompt();
    void                                onSettings();
    void                                onCloseTab( int index );
    void                                onTabChanged( int index );
    void                                onThemeToggle();

    void                                appendLog( const QString& text );

protected:
    bool                                eventFilter( QObject* watched, QEvent* event ) override;
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
    bool                                canPasteClipboardImage() const;
    bool                                canPasteClipboardImageInCurrentContext() const;
    bool                                shouldConfirmClipboardImageOpen() const;
    bool                                confirmOpenClipboardImage() const;
    bool                                openClipboardImage();
    bool                                confirmOpenCapturedImage() const;
    bool                                openCapturedImage( const QImage& image, const QString& title = {} );
    QStringList                         droppedPathsFromMimeData( const QMimeData* mimeData ) const;
    bool                                hasDroppedPaths( const QMimeData* mimeData ) const;
    bool                                handleDropEvent( QDropEvent* event );
    void                                openDroppedPaths( const QStringList& paths );
    void                                openDroppedDirectory( const QString& dirPath );
    QString                             firstImageFileInDirectory( const QString& dirPath ) const;
    bool                                shouldConfirmBinaryTextOpen( const QString& filePath ) const;
    bool                                confirmOpenBinaryTextFile( const QString& filePath ) const;

    QBaseView*                          createViewForFile( const QString& filePath );
    void                                applyPersistedViewSettings( QBaseView* view );
    void                                applySettingsToAllViews();

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
    void                                onOutlineItemActivated( QTreeWidgetItem* item, int column );
    /// 누락된 Sphinx 확장/테마를 알리는 비모달 바.
    /// 프리뷰 빌드는 디바운스 타이머로 발화하므로 모달을 띄우면 쓸 수 없게 된다.
    void                                setupMissingDependencyBar();
    void                                showMissingDependencies( const QStringList& distributions );

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
    QWidget*                            missingDepBar_ = nullptr;
    QLabel*                             missingDepLabel_ = nullptr;
    QStringList                         missingDepPending_;
    QStringList                         missingDepDismissed_;        // 이번 세션에 거절한 것

    QString                             workspaceRoot_;

    // 검색 탭 위젯들. .ui 를 건드리지 않고 코드로 만든다.
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
