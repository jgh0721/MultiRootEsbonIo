#pragma once

#include "core/solBaseView.hpp"
#include "core/solSphinxScanner.hpp"

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
class QTimer;
class QVBoxLayout;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow( QWidget* parent = nullptr );
    ~MainWindow() override;

    void openFile( const QString& filePath );
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
    void applyThemeToView( QBaseView* view ) const;
    void applyCurrentTheme();
    void updateTitle();
    void updateTabDecoration( QBaseView* view );
    void updateViewerToolBar();
    void updateStatusBar();
    void connectViewStatusSignals( QBaseView* view );
    void updateSaveActionState();
    void updateCopyActionState();
    void updatePasteActionState();
    bool saveView( QBaseView* view, bool saveAs );
    void setViewLoadingState( QBaseView* view, bool active, const QString& message, int value, int maximum );
    void refreshLoadingIndicator();
    void advanceLoadingAnimation();
    void onCancelLoading();
    void addRecentFile( const QString& filePath );
    void updateRecentFilesMenu();
    void shutdownUi();
    int addViewTab( QBaseView* view );
    void disconnectViewSignals( QBaseView* view );
    void removeViewTabWithoutSignals( QBaseView* view );
    void teardownView( QBaseView* view );
    void teardownView( QBaseView* view, const ViewTeardownOptions& options );
    void refreshCurrentViewUi();
    QBaseView* preferredLoadingView() const;
    void cancelLoadingView( QBaseView* view, bool showStatusMessage = true );
    QIcon tabIconForView( QBaseView* view ) const;
    bool canPasteClipboardImage() const;
    bool canPasteClipboardImageInCurrentContext() const;
    bool shouldConfirmClipboardImageOpen() const;
    bool confirmOpenClipboardImage() const;
    bool openClipboardImage();
    bool confirmOpenCapturedImage() const;
    bool openCapturedImage( const QImage& image, const QString& title = {} );
    QStringList droppedPathsFromMimeData( const QMimeData* mimeData ) const;
    bool hasDroppedPaths( const QMimeData* mimeData ) const;
    bool handleDropEvent( QDropEvent* event );
    void openDroppedPaths( const QStringList& paths );
    void openDroppedDirectory( const QString& dirPath );
    QString firstImageFileInDirectory( const QString& dirPath ) const;
    bool shouldConfirmBinaryTextOpen( const QString& filePath ) const;
    bool confirmOpenBinaryTextFile( const QString& filePath ) const;

    QBaseView* createViewForFile( const QString& filePath );
    void applyPersistedViewSettings( QBaseView* view );
    void applySettingsToAllViews();

    Ui::MainWindow                      Ui;
    QFileSystemModel*                   treLeftFolderTreeModel_ = nullptr;

    QWidget* m_centralContainer = nullptr;
    QWidget* m_viewerToolBarHost = nullptr;
    QVBoxLayout* m_viewerToolBarLayout = nullptr;
    QTabWidget* m_tabWidget = nullptr;
    QToolBar* m_mainToolBar = nullptr;
    QPointer<QToolBar> m_viewerToolBar = nullptr;   // 뷰어별 도구모음
    QPointer<QToolBar> m_viewerAuxToolBar = nullptr; // 뷰어별 2줄 보조 도구모음
    QLabel* m_loadingLabel = nullptr;
    QProgressBar* m_loadingProgressBar = nullptr;
    QPushButton* m_loadingCancelButton = nullptr;
    QLabel* m_statusLabel = nullptr;   // 상태 표시줄 정보
    QMenu* m_recentMenu = nullptr;   // 최근 파일 메뉴
    QAction* m_saveAction = nullptr;
    QAction* m_saveAsAction = nullptr;
    QAction* m_captureAction = nullptr;
    QAction* m_copyAction = nullptr;
    QAction* m_pasteAction = nullptr;
    QHash<QBaseView*, ViewLoadingState> m_activeViewLoads;
    QTimer* m_loadingAnimationTimer = nullptr;
    int             m_loadingAnimationFrame = 0;
    QStringList     m_recentFiles;
    static constexpr int MaxRecentFiles = 10;
    bool            m_shuttingDown = false;

private:

    ///////////////////////////////////////////////////////////////////////////
    /// Esbonio / Sphinx
    
    void                                setWorkspace( const QString& Folder );
    void                                refreshProjectList();

    QString                             workspaceRoot_;
    std::vector< mrst::SphinxProject>   projects_;
};
