#include "stdafx.h"
#include "MainWindow.hpp"

#include "core/solAppSettings.hpp"
#include "core/solBaseView.hpp"
#include "core/solFileKinds.hpp"
#include "core/solPythonEnvMgr.hpp"
#include "core/solRestWorkspaceController.hpp"
#include "core/solSphinxDiagnosticsStore.hpp"
#include "core/solWorkspaceSearch.hpp"
#include "core/solWorkspaceSession.hpp"
#include "core/solThemeManager.hpp"
#include "core/solShadowBackupStore.hpp"
#include "core/solUpdateService.hpp"
#include "editor/QBaseEditor.hpp"
#include "uis/dlgAbout.hpp"
#include "uis/dlgSettings.hpp"
#include "utils/DwmTitleBar.hpp"
#include "utils/solBackgroundWork.hpp"
#include "utils/solPhaseTrace.hpp"

#include <QWebEnginePage>
#include <QWebEngineScriptCollection>

#include <QActionGroup>
#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDir>
#include <QDebug>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QLayout>
#include <QMimeData>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <QVector>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
    /// 이 파일이 텍스트 편집기로 열리는가.
    ///
    /// 쓰는 곳은 shouldConfirmBinaryTextOpen() 하나다 — 이진 내용을 텍스트로 열려고
    /// 할 때 되묻는 판정이라, "편집기가 아니라 전용 뷰어로 여는 확장자" 를 뺀다.
    ///
    /// `.md` 는 빼지 않는다. 폐기된 전용 마크다운 뷰어 시절의 논리가 남아 있었는데,
    /// 지금 `.md` 는 다른 텍스트 파일과 똑같이 Scintilla 편집기로 열리므로 되묻기를
    /// 건너뛸 이유가 없다.
    bool fileWouldOpenAsText( const QString& filePath )
    {
        const QString ext = QFileInfo( filePath ).suffix().toLower();
        return ext != QStringLiteral( "pdf" )
            && !mrst::filekinds::imageExtensions().contains( ext );
    }

    bool canCloseWithTextHotExit( QBaseView* view )
    {
        return false;
        //auto* textView = qobject_cast< QTextView* >( view );
        //return textView
        //    && textView->isHotExitEnabled()
        //    && !textView->isLimitedPreviewMode()
        //    && !textView->isContentTruncated();
    }

    QString stripOptionalQuotes( QString text )
    {
        text = text.trimmed();
        if( text.size() >= 2 )
        {
            const QChar first = text.front();
            const QChar last = text.back();
            if( ( first == QLatin1Char( '"' ) && last == QLatin1Char( '"' ) )
                || ( first == QLatin1Char( '\'' ) && last == QLatin1Char( '\'' ) ) )
            {
                text = text.mid( 1, text.size() - 2 ).trimmed();
            }
        }
        return text;
    }

    QString pathFromDroppedUrl( const QUrl& url )
    {
        QString path;
        if( url.isLocalFile() || url.scheme().compare( QStringLiteral( "file" ), Qt::CaseInsensitive ) == 0 )
            path = url.toLocalFile();

        if( path.isEmpty() && url.scheme().isEmpty() )
            path = url.toString( QUrl::PreferLocalFile | QUrl::RemoveQuery | QUrl::RemoveFragment );

        return QDir::fromNativeSeparators( stripOptionalQuotes( path ) );
    }

    QString pathFromDroppedTextLine( QString line )
    {
        line = stripOptionalQuotes( line );
        if( line.isEmpty() )
            return {};

        QString nativePath = QDir::fromNativeSeparators( line );
        const bool isUncPath = nativePath.startsWith( QStringLiteral( "//" ) );
        const bool isDrivePath = nativePath.size() >= 3
            && nativePath.at( 1 ) == QLatin1Char( ':' )
            && nativePath.at( 2 ) == QLatin1Char( '/' );
        const bool isAbsolutePath = nativePath.startsWith( QLatin1Char( '/' ) );

        if( isUncPath || isDrivePath || isAbsolutePath )
            return nativePath;

        const QUrl url( line );
        if( url.isValid() && !url.scheme().isEmpty() )
        {
            if( url.isLocalFile() || url.scheme().compare( QStringLiteral( "file" ), Qt::CaseInsensitive ) == 0 )
                return pathFromDroppedUrl( url );
            return {};
        }

        return {};
    }

    QString droppedPathKey( QString path )
    {
        path = QDir::fromNativeSeparators( path.trimmed() );
        return path.toCaseFolded();
    }

    bool hasUtfTextBom( const QByteArray& data )
    {
        const auto byteAt = [&data]( int index ) {
            return static_cast< unsigned char >( data.at( index ) );
            };

        if( data.size() >= 3
            && byteAt( 0 ) == 0xEF && byteAt( 1 ) == 0xBB && byteAt( 2 ) == 0xBF )
            return true;

        if( data.size() >= 4
            && ( ( byteAt( 0 ) == 0xFF && byteAt( 1 ) == 0xFE && byteAt( 2 ) == 0x00 && byteAt( 3 ) == 0x00 )
                 || ( byteAt( 0 ) == 0x00 && byteAt( 1 ) == 0x00 && byteAt( 2 ) == 0xFE && byteAt( 3 ) == 0xFF ) ) )
            return true;

        return data.size() >= 2
            && ( ( byteAt( 0 ) == 0xFF && byteAt( 1 ) == 0xFE )
                || ( byteAt( 0 ) == 0xFE && byteAt( 1 ) == 0xFF ) );
    }

    bool looksLikeUtf16WithoutBom( const QByteArray& data )
    {
        const qsizetype pairs = data.size() / 2;
        if( pairs < 8 )
            return false;

        int evenZeros = 0;
        int oddZeros = 0;
        int evenText = 0;
        int oddText = 0;
        for( int i = 0; i + 1 < data.size(); i += 2 )
        {
            const auto even = static_cast< unsigned char >( data.at( i ) );
            const auto odd = static_cast< unsigned char >( data.at( i + 1 ) );
            if( even == 0 )
                ++evenZeros;
            if( odd == 0 )
                ++oddZeros;
            if( even == '\t' || even == '\n' || even == '\r' || even >= 0x20 )
                ++evenText;
            if( odd == '\t' || odd == '\n' || odd == '\r' || odd >= 0x20 )
                ++oddText;
        }

        return ( oddZeros > pairs * 0.60 && evenText > pairs * 0.60 )
            || ( evenZeros > pairs * 0.60 && oddText > pairs * 0.60 );
    }

    bool isProbablyBinaryFile( const QString& filePath )
    {
        QFile file( filePath );
        if( !file.open( QIODevice::ReadOnly ) )
            return false;

        const QByteArray sample = file.read( 8192 );
        if( sample.isEmpty() || hasUtfTextBom( sample ) || looksLikeUtf16WithoutBom( sample ) )
            return false;

        int nulBytes = 0;
        int controlBytes = 0;
        for( char byte : sample )
        {
            const auto ch = static_cast< unsigned char >( byte );
            if( ch == 0 )
            {
                ++nulBytes;
                continue;
            }
            if( ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f' && ch != '\b' )
                ++controlBytes;
        }

        if( nulBytes > 0 )
            return true;

        return controlBytes > 16 && controlBytes > sample.size() / 10;
    }

    void refreshViewerToolBarSlot( QWidget* host )
    {
        if( !host )
            return;

        if( auto* hostLayout = host->layout() )
        {
            hostLayout->invalidate();
            hostLayout->activate();
        }

        const auto directToolBars = host->findChildren<QToolBar*>( QString(), Qt::FindDirectChildrenOnly );
        host->setVisible( std::any_of( directToolBars.cbegin(), directToolBars.cend(), []( const QToolBar* toolBar ) {
            return toolBar && ( toolBar->objectName() == QStringLiteral( "viewerToolBar" )
                               || toolBar->objectName() == QStringLiteral( "viewerAuxToolBar" ) );
                                       } ) );
        host->updateGeometry();
        host->update();

        if( auto* parentWidget = host->parentWidget() )
        {
            if( auto* parentLayout = parentWidget->layout() )
            {
                parentLayout->invalidate();
                parentLayout->activate();
            }
            parentWidget->updateGeometry();
            parentWidget->update();
        }
    }

    void destroyViewerToolBar( QWidget* host, QPointer<QToolBar>& toolBar )
    {
        if( !host || !toolBar )
            return;

        QToolBar* oldToolBar = toolBar;
        toolBar = nullptr;

        if( auto* layout = host->layout() )
            layout->removeWidget( oldToolBar );

        oldToolBar->hide();
        delete oldToolBar;
        refreshViewerToolBarSlot( host );
    }

    void purgeStaleViewerToolBars( QWidget* host, QToolBar* keep = nullptr, QToolBar* keepAux = nullptr )
    {
        if( !host )
            return;

        const auto staleToolBars = host->findChildren<QToolBar*>( QString(), Qt::FindDirectChildrenOnly );
        for( QToolBar* toolBar : staleToolBars )
        {
            if( !toolBar || toolBar == keep || toolBar == keepAux )
                continue;
            if( toolBar->objectName() != QStringLiteral( "viewerToolBar" )
                && toolBar->objectName() != QStringLiteral( "viewerAuxToolBar" ) )
                continue;

            if( auto* layout = host->layout() )
                layout->removeWidget( toolBar );

            toolBar->hide();
            delete toolBar;
        }

        refreshViewerToolBarSlot( host );
    }

    constexpr int kLoadingAnimationFrameCount = 12;
    constexpr int kLoadingAnimationIntervalMs = 90;
    constexpr qreal kPi = 3.14159265358979323846;

    /// 창이 뜨고 나서 업데이트를 확인하기까지의 여유.
    /// 첫 페인트와 파이썬 부트스트랩 시작이 먼저 지나가게 둔다.
    /// 첫 페인트가 오지 않을 때 기동 단계를 강제로 넘기는 안전망.
    /// 최소화 상태로 시작하면 Paint 이벤트가 오지 않는다.
    constexpr int kStartupPaintFallbackMs = 400;

    constexpr int kUpdateFirstCheckDelayMs = 5000;
    /// 며칠씩 켜 두는 앱이라 기동 시 한 번만 보면 주기 설정이 무의미해진다.
    constexpr int kUpdateHeartbeatMs = 6 * 60 * 60 * 1000;

    QIcon loadingTabIcon( const QIcon& baseIcon, const QPalette& palette, int frame )
    {
        constexpr int canvasSize = 18;
        constexpr int baseSize = 11;
        constexpr qreal radius = 6.0;
        constexpr qreal minDotRadius = 0.9;
        constexpr qreal maxDotRadius = 1.8;

        QPixmap pixmap( canvasSize, canvasSize );
        pixmap.fill( Qt::transparent );

        QPainter painter( &pixmap );
        painter.setRenderHint( QPainter::Antialiasing, true );

        if( !baseIcon.isNull() )
        {
            const QPixmap basePixmap = baseIcon.pixmap( baseSize, baseSize );
            const QPoint topLeft( ( canvasSize - basePixmap.width() ) / 2,
                                 ( canvasSize - basePixmap.height() ) / 2 );
            painter.setOpacity( 0.82 );
            painter.drawPixmap( topLeft, basePixmap );
            painter.setOpacity( 1.0 );
        }

        QColor spinnerColor = palette.highlight().color();
        if( !spinnerColor.isValid() )
            spinnerColor = QColor( 53, 132, 228 );

        const QPointF center( canvasSize / 2.0, canvasSize / 2.0 );
        for( int segment = 0; segment < kLoadingAnimationFrameCount; ++segment )
        {
            const int distance = ( segment - frame + kLoadingAnimationFrameCount ) % kLoadingAnimationFrameCount;
            const qreal emphasis = 1.0 - ( static_cast< qreal >( distance ) / kLoadingAnimationFrameCount );
            QColor dotColor = spinnerColor;
            dotColor.setAlphaF( std::clamp( 0.20 + ( emphasis * 0.72 ), 0.0, 1.0 ) );

            const qreal angle = ( ( static_cast< qreal >( segment ) / kLoadingAnimationFrameCount ) * 2.0 * kPi ) - ( kPi / 2.0 );
            const QPointF dotCenter( center.x() + ( std::cos( angle ) * radius ),
                                    center.y() + ( std::sin( angle ) * radius ) );
            const qreal dotRadius = minDotRadius + ( ( maxDotRadius - minDotRadius ) * emphasis );
            painter.setPen( Qt::NoPen );
            painter.setBrush( dotColor );
            painter.drawEllipse( dotCenter, dotRadius, dotRadius );
        }

        return QIcon( pixmap );
    }

}

// ═══════════════════════════════════════════════════════════
MainWindow::MainWindow( QWidget* parent )
    : QMainWindow( parent )
{
    const mrst::PhaseSpan ctorSpan( "ctor" );
    mrst::traceP( "ui.setupUi.begin" );
    Ui.setupUi( this );
    mrst::traceP( "ui.setupUi.end" );
    setWindowTitle( tr( "MultiRoot reST Editor" ) );
    resize( 1024, 768 );
    setAcceptDrops( true );
    if( auto* app = QCoreApplication::instance() )
        app->installEventFilter( this );

    setupCentralContainer();

    treLeftFolderTreeModel_ = new QFileSystemModel( this );
    treLeftFolderTreeModel_->setRootPath( "" );
    treLeftFolderTreeModel_->setNameFilters( QStringList() << "*.rst" << "*.md" << "*.py" << "*.json" << "*.txt" );
    treLeftFolderTreeModel_->setNameFilterDisables( false );
    Ui.treLeftSideFolterTree->setModel( treLeftFolderTreeModel_ );
    Ui.treLeftSideFolterTree->header()->hideSection( 1 );
    Ui.treLeftSideFolterTree->header()->hideSection( 2 );
    Ui.treLeftSideFolterTree->header()->hideSection( 3 );
    Ui.treLeftSideFolterTree->setIndentation( 15 );

    connect( Ui.treLeftSideFolterTree, &QTreeView::doubleClicked, this, [this]( const QModelIndex& index ) {
        const QFileInfo fileInfo = treLeftFolderTreeModel_->fileInfo( index );
        if( !fileInfo.isFile() )
            return;

        openFile( fileInfo.absoluteFilePath() );
    } );

    m_tabWidget = Ui.tabEditor;
    m_tabWidget->setAcceptDrops( true );
    m_tabWidget->setTabsClosable( true );
    m_tabWidget->setMovable( true );
    m_tabWidget->setDocumentMode( true );
    // WA_OpaquePaintEvent 는 붙이지 않는다. QTabWidget 은 탭 베이스와 프레임만 그려
    // 자기 영역을 전부 채우지 않으므로, 배경 지우기를 끄면 리사이즈 때 잔상이 남는다.

    // 시작 화면(setHtml)과 컨트롤러 연결은 여기서 하지 않는다. 둘 다 page() 를
    // 만들어 Chromium 을 통째로 띄우고(실측 424~1254ms), 그 시점은 첫 프레임보다
    // 앞이라 그동안 창이 아무것도 그리지 못한다. initialisePreview() 로 미룬다.

    Ui.splFolderWithOutlineOnSide->setMinimumWidth( 200 );
    Ui.frmBottom->setMinimumHeight( 150 );
    Ui.splEditWithStatisticsOnContent->setStretchFactor( 0, 1 );
    Ui.splEditWithStatisticsOnContent->setStretchFactor( 1, 0 );
    Ui.splSideWithContent->setStretchFactor( 0, 0 );
    Ui.splSideWithContent->setStretchFactor( 1, 1 );

    // 편집기 | 프리뷰 스플리터.
    // 자식 하나가 QWebEngineView(별도 합성 표면)라 핸들을 끌면 노출 영역이
    // 실시간으로 바뀐다. 두 프레임이 스스로 배경을 칠하게 해야 그 틈이
    // 이전 픽셀(검은 띠)로 남지 않는다.
    Ui.splitter_2->setChildrenCollapsible( false );
    Ui.splitter_2->setStretchFactor( 0, 1 );
    Ui.splitter_2->setStretchFactor( 1, 1 );
    // 최소 폭을 양쪽에 준다.
    //
    // setChildrenCollapsible(false) 는 "핸들을 끌어 접을 수 없다" 는 뜻일 뿐이고
    // 최소 폭이 0 이면 레이아웃 계산에서 0 이 될 수 있다.
    //
    // 편집기 쪽이 더 중요하다. Scintilla 위젯과 열 눈금자가 minimumSizeHint 로
    // 큰 값을 요구해서(실측: 스플리터 폭 864 에서 편집기 743 / 프리뷰 120), 그것을
    // 덮지 않으면 프리뷰가 항상 자기 최소 폭까지 눌린다. setMinimumWidth 는
    // minimumSizeHint 를 덮으므로 이 한 줄이 그 요구를 풀어 준다.
    //
    // 값을 작게 잡는 이유: 두 하한의 합이 창의 최소 폭이 되므로, 크게 잡으면 탭이
    // 열리는 동안 레이아웃이 창을 강제로 넓힌다.
    Ui.frmEditor->setMinimumWidth( 240 );
    Ui.frmWebPreview->setMinimumWidth( 120 );
    Ui.frmEditor->setAutoFillBackground( true );
    Ui.frmWebPreview->setAutoFillBackground( true );

    connect( m_tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onCloseTab );
    connect( m_tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged );

    createMenus();

    m_loadingLabel = new QLabel( this );
    m_loadingLabel->setVisible( false );
    m_loadingProgressBar = new QProgressBar( this );
    m_loadingProgressBar->setVisible( false );
    m_loadingProgressBar->setTextVisible( true );
    m_loadingProgressBar->setAlignment( Qt::AlignCenter );
    m_loadingProgressBar->setFormat( QStringLiteral( "%p%" ) );
    m_loadingProgressBar->setFixedWidth( 180 );
    m_loadingCancelButton = new QPushButton( tr( "취소" ), this );
    m_loadingCancelButton->setVisible( false );
    m_loadingCancelButton->setAutoDefault( false );
    m_loadingCancelButton->setDefault( false );
    connect( m_loadingCancelButton, &QPushButton::clicked, this, &MainWindow::onCancelLoading );
    statusBar()->addPermanentWidget( m_loadingLabel );
    statusBar()->addPermanentWidget( m_loadingProgressBar );
    statusBar()->addPermanentWidget( m_loadingCancelButton );

    m_statusLabel = new QLabel( this );
    statusBar()->addPermanentWidget( m_statusLabel );
    statusBar()->showMessage( tr( "Ready" ) );

    m_loadingAnimationTimer = new QTimer( this );
    m_loadingAnimationTimer->setInterval( kLoadingAnimationIntervalMs );
    connect( m_loadingAnimationTimer, &QTimer::timeout, this, &MainWindow::advanceLoadingAnimation );

    // 최근 파일 복원
    AppSettings settings;
    m_recentFiles = settings.value( "recentFiles" ).toStringList();
    updateRecentFilesMenu();

    if( auto* clipboard = QApplication::clipboard() )
        connect( clipboard, &QClipboard::dataChanged, this, &MainWindow::updatePasteActionState );

    connect( &ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this]( ThemeManager::Theme ) { applyCurrentTheme(); } );
    applyCurrentTheme();
    updateSaveActionState();
    updateCopyActionState();
    updatePasteActionState();

    // 저장된 단축키 적용
    {
        const auto shortcuts = QSettingsDialog::LoadShortcutsFromSettings();
        QSettingsDialog::ApplyShortcutsToActions( shortcuts, this );
    }

    // Sphinx/Esbonio 조율자. 탭이 복원되기 전에 먼저 만들어 둔다.
    // 프리뷰 위젯 주입은 initialisePreview() 에서 한다 — 그것이 Chromium 을 띄운다.
    controller_ = new mrst::WorkspaceController( this );
    connect( controller_, &mrst::WorkspaceController::logMessage, this, &MainWindow::appendLog );
    connect( controller_, &mrst::WorkspaceController::navigateRequested, this,
            [this]( const QString& path, const int line, const int column ) {
                openFile( path );
                if( QTextView* view = textViewOf( currentView() ) )
                    view->goToPosition( line, column );
            } );

    setupDiagnosticsTable();
    setupOutlineTrees();
    setupWorkspaceSearchTab();
    setupMissingDependencyBar();
    setupPythonEnvironment();
    setupUpdateBar();
    setupUpdateService();

    connect( controller_, &mrst::WorkspaceController::missingDependenciesDetected, this,
            [this]( const QString&, const QStringList& distributions, const QStringList& themes ) {
                showMissingDependencies( distributions + themes );
            } );

    // hot-exit 복원과 세션 복원은 advanceStartupPhase() 가 첫 페인트 뒤에 한다.
    // 백업 디렉터리 전량 스캔 + 탭 생성이 전부 첫 프레임 앞을 막던 자리다.
}

void MainWindow::setStartupPaths( const QStringList& paths )
{
    startupPaths_ = paths;
}

void MainWindow::showEvent( QShowEvent* event )
{
    QMainWindow::showEvent( event );

    // 최소화 상태로 시작하는 등 Paint 가 오지 않는 경우가 있다. 어느 쪽이 먼저든
    // advanceStartupPhase() 는 멱등이라 한 번만 실행된다.
    QTimer::singleShot( kStartupPaintFallbackMs, this, &MainWindow::advanceStartupPhase );
}

void MainWindow::advanceStartupPhase()
{
    if( startupPhase_ != StartupPhase::Shell || m_shuttingDown )
        return;
    startupPhase_ = StartupPhase::Ready;

    const mrst::PhaseSpan span( "phase.ready" );

    // 프리뷰를 **복원보다 먼저** 붙인다. 탭이 열리면서 곧바로 프리뷰 빌드가
    // 요청되는데, 그때 previewView_ 가 없으면 결과가 조용히 버려진다
    // (WorkspaceController::onPreviewFinished 의 조기 반환).
    initialisePreview();

    restoreHotExitSnapshots();

    if( !startupPaths_.isEmpty() )
        openStartupPaths( startupPaths_ );
    else
        restoreLastSession();

    // 세션이 배치를 정하지 않는 경로가 둘 있다 — 명령줄 인자로 기동한 경우와,
    // `.multiroot/workspace.json` 이 아직 없는 새 워크스페이스다. 그러면 스플리터가
    // Qt 기본 배분에 맡겨지고 프리뷰가 0 폭으로 시작한다(핸들을 끌면 되살아나므로
    // "프리뷰 기능이 없다" 로 오인하기 쉽다). 탭을 다 연 뒤에 손본다.
    // 레이아웃이 안정된 뒤에 손본다. 탭이 열리는 동안에는 스플리터가 아직 최종
    // 폭을 모른다.
    QTimer::singleShot( 0, this, &MainWindow::ensureVisiblePreviewSplit );
}

void MainWindow::ensureVisiblePreviewSplit()
{
    // 세션이 배치를 정했으면 손대지 않는다. 사용자가 좁혀 둔 프리뷰를 되돌리면
    // 그 조작이 매 실행마다 사라진다.
    if( previewSplitFromSession_ )
        return;

    QSplitter* splitter = Ui.splitter_2;
    if( splitter == nullptr )
        return;

    const QList< int > sizes = splitter->sizes();
    if( sizes.size() != 2 )
        return;

    const int total = sizes.at( 0 ) + sizes.at( 1 );
    if( total <= 0 )
        return;   // 아직 레이아웃이 없다

    splitter->setSizes( { total / 2, total - total / 2 } );
}

void MainWindow::initialisePreview()
{
    if( previewInitialised_ || Ui.webEngineView == nullptr )
        return;
    previewInitialised_ = true;

    const mrst::PhaseSpan span( "preview.init" );

    //: 문서를 열기 전 프리뷰 영역에 보이는 시작 화면. <h1>/<p> 태그는 그대로 둘 것.
    Ui.webEngineView->setHtml( tr( "<h1>MultiRoot reST</h1><p>셸이 시작되었습니다.</p>" ) );

    // 생성자의 applyCurrentTheme() 는 page() 가 없어 바탕색을 못 칠했다. 지금 칠한다.
    Ui.webEngineView->page()->setBackgroundColor( ThemeManager::instance().backgroundColor() );

    if( controller_ )
        controller_->setPreviewView( Ui.webEngineView );
}

void MainWindow::restoreHotExitSnapshots()
{
    if( !AppSettings().value( "textView/hotExitEnabled", true ).toBool() )
        return;

    const mrst::PhaseSpan hotExitSpan( "hotexit.scan" );
    const QList<TextShadowBackupStore::Snapshot> hotExitSnapshots = TextShadowBackupStore::restorableSnapshots( false );
    mrst::traceP( "hotexit.snapshots", QString::number( hotExitSnapshots.size() ) );
    for( const TextShadowBackupStore::Snapshot& snapshot : hotExitSnapshots )
    {
        if( snapshot.isUntitled )
        {
            auto* view = new QTextView( this );
            applyPersistedViewSettings( view );
            applyThemeToView( view );
            if( !view->openHotExitBackup( snapshot.untitledId ) )
            {
                delete view;
                continue;
            }
            addViewTab( view );
            continue;
        }

        openFile( snapshot.originalFilePath );
    }
}

MainWindow::~MainWindow()
{
    if( auto* app = QCoreApplication::instance() )
        app->removeEventFilter( this );
    shutdownUi();
}

// ═══════════════════════════════════════════════════════════
// 메뉴
// ═══════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════
// 메뉴는 만들 때와 언어를 바꿀 때가 같은 문자열 표를 쓴다.
//
// createMenus() 는 구조/단축키/연결만 만들고 표시 문자열은 하나도 쓰지 않는다.
// 문자열은 전부 retranslateMenus() 한 곳에 있고, createMenus() 가 마지막에
// 그것을 부른다. 문자열이 두 벌로 갈라질 수 없고, 항목을 추가하고 표에 넣는 것을
// 잊으면 한국어에서도 빈 메뉴로 즉시 드러난다.
//
// **메뉴를 다시 만들지 않는 이유**: menuBar()->clear() 는 QMenu 객체를 지우지
// 않는다. 옛 QAction 이 MainWindow 자손으로 살아남고, 그 대부분이
// Qt::ApplicationShortcut 이다. QSettingsDialog::ApplyShortcutsToActions() 가
// findChildren<QAction*>() 로 옛것과 새것 모두에 같은 단축키를 걸면 Qt 는
// 모호한 단축키로 보고 **둘 다 발동시키지 않는다** — 언어를 한 번 바꾸면
// Ctrl+S 가 죽는다. (같은 함정이 QBaseEditor.cpp 에도 적혀 있다.)
// ═══════════════════════════════════════════════════════════
void MainWindow::createMenus()
{
    auto* fileMenu = menuBar()->addMenu( QString() );
    fileMenu->setObjectName( QStringLiteral( "menu.file" ) );

    auto* newAction = fileMenu->addAction( QString(), this, &MainWindow::onFileNew );
    newAction->setObjectName( QStringLiteral( "file.new" ) );
    newAction->setProperty( "mv.shortcutId", QStringLiteral( "file.new" ) );
    newAction->setShortcut( QKeySequence::New );
    newAction->setShortcutContext( Qt::ApplicationShortcut );

    auto* openAction = fileMenu->addAction( QString(), this, &MainWindow::onFileOpen );
    openAction->setObjectName( QStringLiteral( "file.open" ) );

    auto* openWorkspace = fileMenu->addAction( QString(), QKeySequence::Open, this, &MainWindow::onWorkspaceOpen );
    openWorkspace->setObjectName( QStringLiteral( "file.openWorkspace" ) );
    openWorkspace->setProperty( "mv.shortcutId", QStringLiteral( "file.openWorkspace" ) );
    openWorkspace->setShortcut( QKeySequence::Open );
    openWorkspace->setShortcutContext( Qt::ApplicationShortcut );

    m_saveAction = fileMenu->addAction( QString(), this, &MainWindow::onFileSave );
    m_saveAction->setObjectName( QStringLiteral( "file.save" ) );
    m_saveAction->setProperty( "mv.shortcutId", QStringLiteral( "file.save" ) );
    m_saveAction->setShortcut( QKeySequence::Save );
    m_saveAction->setShortcutContext( Qt::ApplicationShortcut );

    m_saveAsAction = fileMenu->addAction( QString(), this, &MainWindow::onFileSaveAs );
    m_saveAsAction->setObjectName( QStringLiteral( "file.saveAs" ) );
    m_saveAsAction->setProperty( "mv.shortcutId", QStringLiteral( "file.saveAs" ) );
    m_saveAsAction->setShortcut( QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_S ) );
    m_saveAsAction->setShortcutContext( Qt::ApplicationShortcut );

    m_recentMenu = fileMenu->addMenu( QString() );
    m_recentMenu->setObjectName( QStringLiteral( "menu.recent" ) );

    fileMenu->addSeparator();
    auto* closeTabAction = fileMenu->addAction( QString(), this, [this] {
        if( m_tabWidget )
            onCloseTab( m_tabWidget->currentIndex() );
    } );
    closeTabAction->setObjectName( QStringLiteral( "tab.close" ) );
    closeTabAction->setProperty( "mv.shortcutId", QStringLiteral( "tab.close" ) );
    closeTabAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_W ) );
    closeTabAction->setShortcutContext( Qt::ApplicationShortcut );

    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction( QString(), QKeySequence::Quit, QApplication::instance(),
                                            &QApplication::quit );
    quitAction->setObjectName( QStringLiteral( "app.quit" ) );

    auto* editMenu = menuBar()->addMenu( QString() );
    editMenu->setObjectName( QStringLiteral( "menu.edit" ) );
    m_copyAction = editMenu->addAction( QString(), this, &MainWindow::onCopy );
    // objectName 이 없으면 retranslateMenus() 가 찾지 못한다. 겸사겸사 단축키
    // 설정 표에 편집 항목을 넣을 수 있는 자리도 생긴다.
    m_copyAction->setObjectName( QStringLiteral( "edit.copy" ) );
    m_copyAction->setShortcut( QKeySequence::Copy );
    m_copyAction->setShortcutContext( Qt::ApplicationShortcut );
    m_copyAction->setEnabled( false );
    m_pasteAction = editMenu->addAction( QString(), this, &MainWindow::onPaste );
    m_pasteAction->setObjectName( QStringLiteral( "edit.paste" ) );
    m_pasteAction->setShortcut( QKeySequence::Paste );
    m_pasteAction->setShortcutContext( Qt::ApplicationShortcut );
    m_pasteAction->setEnabled( false );

    editMenu->addSeparator();
    auto* completionAction = editMenu->addAction( QString(), this, [this] {
        if( controller_ != nullptr )
            controller_->requestCompletion();
    } );
    completionAction->setObjectName( QStringLiteral( "editor.completion" ) );
    completionAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_Space ) );
    completionAction->setShortcutContext( Qt::WindowShortcut );

    auto* viewMenu = menuBar()->addMenu( QString() );
    viewMenu->setObjectName( QStringLiteral( "menu.view" ) );
    auto* themeToggleAction = viewMenu->addAction( QString(), this, &MainWindow::onThemeToggle );
    themeToggleAction->setObjectName( QStringLiteral( "view.themeToggle" ) );

    // 코드 접기. 마진의 [-] 를 하나씩 누르는 것 말고 문서 전체를 한 번에
    // 여닫는 수단이 있어야 개요처럼 쓸 수 있다. 접기 자체를 켜고 끄는 것은
    // 설정(텍스트 편집기 > 코드 폴딩)에 있다.
    viewMenu->addSeparator();
    auto* foldAllAction = viewMenu->addAction( QString(), this, [this] {
        if( QTextView* view = textViewOf( currentView() ) )
            view->foldAll( true );
    } );
    foldAllAction->setObjectName( QStringLiteral( "editor.foldAll" ) );
    foldAllAction->setShortcut( QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_Minus ) );
    foldAllAction->setShortcutContext( Qt::WindowShortcut );

    auto* unfoldAllAction = viewMenu->addAction( QString(), this, [this] {
        if( QTextView* view = textViewOf( currentView() ) )
            view->foldAll( false );
    } );
    unfoldAllAction->setObjectName( QStringLiteral( "editor.unfoldAll" ) );
    unfoldAllAction->setShortcut( QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_Plus ) );
    unfoldAllAction->setShortcutContext( Qt::WindowShortcut );

    // 프리뷰는 입력 파일이 바뀌지 않으면 다시 빌드하지 않는다(설정
    // preview/skipUnchangedBuild). 그 판정이 놓치는 입력이 있을 수 있으므로
    // 사용자가 강제할 수단이 반드시 있어야 한다.
    viewMenu->addSeparator();
    auto* rebuildPreviewAction = viewMenu->addAction( QString(), this, [this] {
        if( controller_ )
            controller_->requestPreviewBuild( /*immediate=*/true, /*forceRebuild=*/true );
    } );
    rebuildPreviewAction->setObjectName( QStringLiteral( "preview.rebuild" ) );
    rebuildPreviewAction->setProperty( "mv.shortcutId", QStringLiteral( "preview.rebuild" ) );
    rebuildPreviewAction->setShortcut( QKeySequence( Qt::Key_F5 ) );
    rebuildPreviewAction->setShortcutContext( Qt::ApplicationShortcut );

    auto* settingsMenu = menuBar()->addMenu( QString() );
    settingsMenu->setObjectName( QStringLiteral( "menu.settings" ) );
    auto* settingsAction = settingsMenu->addAction( QString(), this, &MainWindow::onSettings );
    settingsAction->setObjectName( QStringLiteral( "app.settings" ) );
    settingsAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_I ) );
    settingsAction->setShortcutContext( Qt::ApplicationShortcut );

    // 업데이트 확인은 도움말이 아니라 여기에 둔다. 확인 주기 설정이 바로 위에
    // 있고, 누르면 결과가 상태 표시줄/알림 바로 나오는 **동작**이라 정보 대화
    // 상자와는 성격이 다르다.
    settingsMenu->addSeparator();
    auto* checkUpdateAction = settingsMenu->addAction( QString(), this, [this] {
        if( updateService_ != nullptr )
            updateService_->checkAsync( /*userInitiated=*/true );
    } );
    checkUpdateAction->setObjectName( QStringLiteral( "app.checkUpdate" ) );

    auto* helpMenu = menuBar()->addMenu( QString() );
    helpMenu->setObjectName( QStringLiteral( "menu.help" ) );
    auto* aboutAction = helpMenu->addAction( QString(), this, &MainWindow::onAbout );
    aboutAction->setObjectName( QStringLiteral( "app.about" ) );
    // 단축키를 주지 않는다. 그러므로 mv.shortcutId 도 붙이지 않는다 —
    // QSettingsDialog::DefaultShortcuts() 표에 없는 Id 는 아무 효과가 없어서
    // "재정의할 수 있다" 는 착각만 남긴다.

    retranslateMenus();
}

void MainWindow::changeEvent( QEvent* event )
{
    // 번역기를 갈아 끼우면 QApplication 이 LanguageChange 를 **모든 최상위
    // 위젯에 post** 하고, 각 QWidget::event() 가 자식 트리로 재귀 전달한다.
    // 그래서 여기 하나만 잡으면 된다 — 생성자에서 건 앱 전역 eventFilter 에서
    // 처리하면 위젯 수만큼 호출되고, updateViewerToolBar() 가 지금 이벤트를
    // 받고 있는 툴바를 delete(=deleteLater 가 아니다) 해 버린다.
    if( event != nullptr && event->type() == QEvent::LanguageChange )
        retranslateUi();

    // 부모 구현을 삼키지 않는다. StyleChange / FontChange / WindowStateChange 도
    // 같은 함수로 들어온다.
    QMainWindow::changeEvent( event );
}

void MainWindow::retranslateUi()
{
    // ⚠ mainWindow.ui 의 windowTitle 이 플레이스홀더 "MainWindow" 라서 이 호출이
    //    창 제목을 덮어쓴다. 아래에서 updateTitle() 로 되돌린다.
    //    (여기서 살아나는 것: 개요 탭 "활성 문서"/"프로젝트", 하단 탭 "진단"/"로그".)
    Ui.retranslateUi( this );

    retranslateMenus();
    updateRecentFilesMenu();            // "(없음)" 항목

    if( Ui.tblDiagnostics != nullptr )
    {
        Ui.tblDiagnostics->setHorizontalHeaderLabels( { tr( "심각도" ), tr( "파일" ), tr( "줄" ),
                                                        tr( "메시지" ), tr( "출처" ) } );
    }

    retranslateWorkspaceSearchTab();

    // 개요 트리는 플레이스홀더를 보여 주는 중일 때만 손댄다. 실제 심볼이
    // 들어 있으면 그건 문서 내용이라 번역 대상이 아니다.
    retranslateOutlinePlaceholders();

    if( missingDepBar_ != nullptr )
    {
        if( auto* b = missingDepBar_->findChild< QPushButton* >( QStringLiteral( "missingDep.install" ) ) )
            b->setText( tr( "설치" ) );
        if( auto* b = missingDepBar_->findChild< QPushButton* >( QStringLiteral( "missingDep.ignore" ) ) )
            b->setText( tr( "무시" ) );
        if( missingDepBar_->isVisible() && !missingDepPending_.isEmpty() )
        {
            missingDepLabel_->setText( tr( "Sphinx 확장/테마를 찾을 수 없습니다: %1" )
                                          .arg( missingDepPending_.join( QStringLiteral( ", " ) ) ) );
        }
    }

    retranslateUpdateBar();

    // 뷰어 도구모음은 QTextView 가 통째로 다시 만든다. 편집기 라벨(언어/인코딩/
    // 줄바꿈/탭 간격)이 여기서 한꺼번에 해결된다 — 테마 전환이 이미 같은 경로를
    // 쓰고 있다.
    updateViewerToolBar();

    updateEnvStatusChip();
    if( m_loadingCancelButton != nullptr )
        m_loadingCancelButton->setText( tr( "취소" ) );

    // Ui.retranslateUi() 가 덮어쓴 창 제목을 되돌린다. 순서가 뒤바뀌면 제목
    // 표시줄에 "MainWindow" 가 남는다.
    updateTitle();
}

void MainWindow::retranslateWorkspaceSearchTab()
{
    if( searchTabPage_ == nullptr || Ui.tabStatistics == nullptr )
        return;

    const int index = Ui.tabStatistics->indexOf( searchTabPage_ );
    if( index >= 0 )
        Ui.tabStatistics->setTabText( index, tr( "검색" ) );

    if( auto* b = searchTabPage_->findChild< QPushButton* >( QStringLiteral( "search.find" ) ) )
        b->setText( tr( "찾기" ) );
    if( auto* b = searchTabPage_->findChild< QPushButton* >( QStringLiteral( "search.preview" ) ) )
        b->setText( tr( "바꾸기 미리보기" ) );
    if( searchApplyButton_ != nullptr )
        searchApplyButton_->setText( tr( "적용" ) );
    if( searchQueryEdit_ != nullptr )
        searchQueryEdit_->setPlaceholderText( tr( "찾을 내용" ) );
    if( searchReplaceEdit_ != nullptr )
        searchReplaceEdit_->setPlaceholderText( tr( "바꿀 내용" ) );
    if( searchCaseBox_ != nullptr )
        searchCaseBox_->setText( tr( "대/소문자 구분" ) );
    if( searchWordBox_ != nullptr )
        searchWordBox_->setText( tr( "단어 단위" ) );
    if( searchRegexBox_ != nullptr )
        searchRegexBox_->setText( tr( "정규식" ) );
    // 상태 라벨은 마지막 검색 결과라 지운다. 개수를 되읽을 방법이 없고, 낡은
    // 언어로 남겨 두는 것보다 비워 두는 편이 정직하다.
    if( searchStatusLabel_ != nullptr && !searchStatusLabel_->text().isEmpty() )
        searchStatusLabel_->setText( tr( "워크스페이스와 찾을 내용을 지정하세요." ) );
}

void MainWindow::retranslateUpdateBar()
{
    if( updateBar_ == nullptr )
        return;

    updateNotesButton_->setText( tr( "릴리스 노트" ) );
    updateSkipButton_ ->setText( tr( "이 버전 건너뛰기" ) );
    updateLaterButton_->setText( tr( "나중에" ) );

    // 라벨과 실행 버튼은 상태에 따라 문구가 다르다. 떠 있을 때만 상태에서 다시
    // 만든다 (숨겨져 있으면 다음에 뜰 때 새 언어로 채워진다).
    if( !updateBar_->isVisible() || updateService_ == nullptr )
        return;
    if( updateService_->state() == mrst::UpdateService::State::ReadyToInstall )
        showUpdateReady( updateService_->available().version );
    else
        showUpdateAvailable( updateService_->available() );
}

void MainWindow::retranslateMenus()
{
    // 검색 범위를 menuBar() 로 좁힌다. MainWindow 전체에서 찾으면 편집기가 만든
    // QAction 과 objectName 이 겹칠 수 있다.
    const auto menuTitle = [this]( const char* name, const QString& title ) {
        if( auto* menu = menuBar()->findChild< QMenu* >( QLatin1String( name ) ) )
            menu->setTitle( title );
    };
    const auto actionText = [this]( const char* name, const QString& text ) {
        if( auto* action = menuBar()->findChild< QAction* >( QLatin1String( name ) ) )
            action->setText( text );
    };

    menuTitle ( "menu.file",          tr( "파일(&F)" ) );
    actionText( "file.new",           tr( "새 파일(&N)" ) );
    actionText( "file.open",          tr( "열기..." ) );
    actionText( "file.openWorkspace", tr( "워크스페이스 열기(&O)..." ) );
    actionText( "file.save",          tr( "저장(&S)" ) );
    actionText( "file.saveAs",        tr( "다른 이름으로 저장(&A)..." ) );
    menuTitle ( "menu.recent",        tr( "최근 파일(&R)" ) );
    actionText( "tab.close",          tr( "현재 탭 닫기(&C)" ) );
    actionText( "app.quit",           tr( "종료(&X)" ) );

    menuTitle ( "menu.edit",          tr( "편집(&E)" ) );
    actionText( "edit.copy",          tr( "복사(&C)" ) );
    actionText( "edit.paste",         tr( "붙여넣기(&P)" ) );
    actionText( "editor.completion",  tr( "자동 완성(&M)" ) );

    menuTitle ( "menu.view",          tr( "보기(&V)" ) );
    actionText( "view.themeToggle",   tr( "테마 전환" ) );
    actionText( "editor.foldAll",     tr( "모두 접기(&F)" ) );
    actionText( "editor.unfoldAll",   tr( "모두 펼치기(&U)" ) );
    actionText( "preview.rebuild",    tr( "프리뷰 다시 빌드(&R)" ) );

    menuTitle ( "menu.settings",      tr( "설정(&S)" ) );
    actionText( "app.settings",       tr( "설정(&I)..." ) );
    actionText( "app.checkUpdate",    tr( "업데이트 확인(&U)..." ) );

    menuTitle ( "menu.help",          tr( "도움말(&H)" ) );
    actionText( "app.about",          tr( "정보(&A)..." ) );
}

// ═══════════════════════════════════════════════════════════
// 중앙 컨테이너 구성
//
// 메뉴 바 바로 아래에 뷰어 도구모음 슬롯을 두고, 그 밑에 .ui 로 만든
// 원래 중앙 위젯을 배치한다. 이 슬롯이 없으면 updateViewerToolBar() 가
// 도구모음을 부모 없는 최상위 위젯으로 만들어 별도 창으로 떠 버린다.
// ═══════════════════════════════════════════════════════════
void MainWindow::setupCentralContainer()
{
    // takeCentralWidget() 은 소유권만 넘기고 삭제하지 않는다.
    // setCentralWidget() 은 기존 중앙 위젯을 삭제하므로 반드시 먼저 떼어낸다.
    QWidget* uiCentralWidget = takeCentralWidget();

    m_centralContainer = new QWidget( this );
    m_centralContainer->setObjectName( QStringLiteral( "centralContainer" ) );
    m_centralContainer->setAcceptDrops( true );

    auto* containerLayout = new QVBoxLayout( m_centralContainer );
    containerLayout->setContentsMargins( 0, 0, 0, 0 );
    containerLayout->setSpacing( 0 );

    m_viewerToolBarHost = new QWidget( m_centralContainer );
    m_viewerToolBarHost->setObjectName( QStringLiteral( "viewerToolBarHost" ) );
    m_viewerToolBarHost->setSizePolicy( QSizePolicy::Preferred, QSizePolicy::Fixed );
    m_viewerToolBarHost->setVisible( false );   // 도구모음이 붙을 때만 보인다

    m_viewerToolBarLayout = new QVBoxLayout( m_viewerToolBarHost );
    m_viewerToolBarLayout->setContentsMargins( 0, 0, 0, 0 );
    m_viewerToolBarLayout->setSpacing( 0 );

    containerLayout->addWidget( m_viewerToolBarHost );

    if( uiCentralWidget )
    {
        uiCentralWidget->setAcceptDrops( true );
        // WA_OpaquePaintEvent 는 붙이지 않는다 — paintEvent 가 없는 평범한 QWidget 이라
        // 배경 지우기를 끄면 노출 영역에 이전 픽셀이 남는다 (스플리터 드래그 시 검은 띠).
        containerLayout->addWidget( uiCentralWidget, 1 );
    }

    setCentralWidget( m_centralContainer );
}

// ═══════════════════════════════════════════════════════════
// 뷰어별 도구모음 교체
// ═══════════════════════════════════════════════════════════
void MainWindow::updateViewerToolBar()
{
    // 이전 뷰어 도구모음 즉시 제거 및 삭제
    if( m_viewerAuxToolBar )
        destroyViewerToolBar( m_viewerToolBarHost, m_viewerAuxToolBar );
    if( m_viewerToolBar )
        destroyViewerToolBar( m_viewerToolBarHost, m_viewerToolBar );

    purgeStaleViewerToolBars( m_viewerToolBarHost );

    auto* view = currentView();
    if( !view )
    {
        refreshViewerToolBarSlot( m_viewerToolBarHost );
        return;
    }

    if( view->isLoading() )
    {
        if( m_viewerToolBarHost )
            m_viewerToolBarHost->setVisible( false );
        refreshViewerToolBarSlot( m_viewerToolBarHost );
        return;
    }

    // 도구모음 슬롯이 없으면 부모 없는 최상위 위젯이 되어 별도 창으로 떠 버린다.
    if( !m_viewerToolBarHost || !m_viewerToolBarLayout )
        return;

    m_viewerToolBar = view->createToolBar();
    if( m_viewerToolBar )
    {
        m_viewerToolBar->setParent( m_viewerToolBarHost );
        m_viewerToolBar->setObjectName( "viewerToolBar" );

        connect( m_viewerToolBar, &QObject::destroyed, this, [this] {
            m_viewerToolBar = nullptr;
        } );

        if( m_viewerToolBarLayout )
            m_viewerToolBarLayout->addWidget( m_viewerToolBar );

        purgeStaleViewerToolBars( m_viewerToolBarHost, m_viewerToolBar );
        if( m_viewerToolBarHost )
            m_viewerToolBarHost->setVisible( true );

        m_viewerToolBar->show();

        m_viewerAuxToolBar = view->createAuxiliaryToolBar();
        if( m_viewerAuxToolBar )
        {
            m_viewerAuxToolBar->setParent( m_viewerToolBarHost );
            m_viewerAuxToolBar->setObjectName( "viewerAuxToolBar" );
            connect( m_viewerAuxToolBar, &QObject::destroyed, this, [this] {
                m_viewerAuxToolBar = nullptr;
            } );
            if( m_viewerToolBarLayout )
                m_viewerToolBarLayout->addWidget( m_viewerAuxToolBar );
        }

        purgeStaleViewerToolBars( m_viewerToolBarHost, m_viewerToolBar, m_viewerAuxToolBar );
        refreshViewerToolBarSlot( m_viewerToolBarHost );
    }
}

// ═══════════════════════════════════════════════════════════
// 파일 열기
// ═══════════════════════════════════════════════════════════
void MainWindow::openFile( const QString& filePath )
{
    const QString normalizedPath = normalizeFilePath( filePath );
    if( normalizedPath.isEmpty() ) return;

    // 창이 뜬 직후(기동 단계가 아직 Shell 일 때) 사용자가 메뉴나 드롭으로 파일을
    // 열 수 있다. 프리뷰가 아직 없으면 곧바로 요청될 빌드 결과가 조용히 버려진다.
    initialisePreview();

    for( int i = 0; i < m_tabWidget->count(); ++i )
    {
        auto* view = dynamic_cast< QBaseView* >( m_tabWidget->widget( i ) );
        if( view && normalizeFilePath( view->currentFilePath() ) == normalizedPath )
        {
            m_tabWidget->setCurrentIndex( i );
            return;
        }
    }

    if( shouldConfirmBinaryTextOpen( normalizedPath ) && !confirmOpenBinaryTextFile( normalizedPath ) )
        return;
    mrst::traceP( "tab.open", QFileInfo( normalizedPath ).fileName() );

    QBaseView* view = createViewForFile( normalizedPath );
    if( !view )
    {
        QMessageBox::warning( this, tr( "오류" ),
                             tr( "지원하지 않는 파일 형식입니다:\n%1" ).arg( normalizedPath ) );
        return;
    }

    //connect( view, &QBaseView::fileOpened, this, [this]( const QString& openedPath ) {
    //    const QString normalizedOpenedPath = normalizeFilePath( openedPath );
    //    if( !normalizedOpenedPath.isEmpty() )
    //        addRecentFile( normalizedOpenedPath );
    //} );
    //connect( view, &QBaseView::fileOpenFailed, this,
    //        [this, view]( const QString& failedPath, const QString& errorMessage ) {
    //            const QString effectivePath = failedPath.isEmpty()
    //                ? view->currentFilePath()
    //                : failedPath;
    //            const QString effectiveMessage = errorMessage.isEmpty()
    //                ? tr( "파일을 열 수 없습니다:\n%1" ).arg( effectivePath )
    //                : errorMessage;

    //            statusBar()->showMessage( effectiveMessage, 4000 );
    //            QMessageBox::warning( this, tr( "오류" ), effectiveMessage );

    //            teardownView( view );
    //            refreshCurrentViewUi();
    //        } );

    connectViewStatusSignals( view );

    // 파일 열기 전에 설정에서 기본값 적용 (뷰 모드, 글꼴 등)
    applyPersistedViewSettings( view );

    const bool asyncOpen = view->opensFileAsynchronously();
    if( asyncOpen )
    {
        applyThemeToView( view );
    }

    if( !view->openFile( normalizedPath ) )
    {
        if( asyncOpen )
        {
            ViewTeardownOptions teardownOptions;
            teardownOptions.deleteLater = false;
            teardownView( view, teardownOptions );
            delete view;
            refreshCurrentViewUi();
            return;
        }

        QMessageBox::warning( this, tr( "오류" ),
                             tr( "파일을 열 수 없습니다:\n%1" ).arg( normalizedPath ) );
        delete view;
        return;
    }

    if( asyncOpen )
    {
        addViewTab( view );
    }
    else
    {
        applyThemeToView( view );
        addViewTab( view );
    }
}

QString MainWindow::normalizeFilePath( const QString& filePath ) const
{
    if( filePath.isEmpty() )
        return {};

    const QFileInfo info( filePath );
    const QString canonicalPath = info.canonicalFilePath();
    if( !canonicalPath.isEmpty() )
        return canonicalPath;

    return QDir::cleanPath( info.absoluteFilePath() );
}

void MainWindow::applyThemeToView( QBaseView* view ) const
{
    if( !view )
        return;

    const auto theme = ThemeManager::instance().currentTheme() == ThemeManager::Dark
        ? QBaseView::Theme::Dark
        : QBaseView::Theme::Light;
    view->setTheme( theme );
}

void MainWindow::applyCurrentTheme()
{
    auto& themeManager = ThemeManager::instance();
    DwmTitleBar::applyTheme( this,
                            themeManager.currentTheme() == ThemeManager::Dark,
                            themeManager.toolBarColor() );

    // 프리뷰가 아직 아무것도 안 그렸거나 리사이즈로 새 영역이 드러났을 때
    // Chromium 이 칠하는 바탕색. 기본값(흰색)이면 다크 테마에서 번쩍인다.
    //
    // previewInitialised_ 를 먼저 보는 이유: QWebEngineView::page() 는 페이지가
    // 없으면 **만들어 버린다.** 즉 `page() != nullptr` 은 검사가 아니라 생성이고,
    // 생성자에서 이 함수를 부르는 순간 Chromium 이 통째로 뜬다. 초기화 전에는
    // 건드리지 않고, initialisePreview() 가 같은 색을 칠한다.
    if( previewInitialised_ && Ui.webEngineView != nullptr )
        Ui.webEngineView->page()->setBackgroundColor( themeManager.backgroundColor() );

    for( int i = 0; i < m_tabWidget->count(); ++i )
    {
        if( auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( i ) ) )
        {
            applyThemeToView( view );
            updateTabDecoration( view );
        }
    }

    updateViewerToolBar();
    //ToolbarIcons::refreshToolBar( m_mainToolBar );
    //ToolbarIcons::refreshToolBar( m_viewerToolBar );
    //ToolbarIcons::refreshToolBar( m_viewerAuxToolBar );
    //ToolbarIcons::refreshAction( m_saveAction );
    //ToolbarIcons::refreshAction( m_saveAsAction );
    //ToolbarIcons::refreshAction( m_captureAction );
    //ToolbarIcons::refreshAction( m_captureProtectionOffAction );
    //ToolbarIcons::refreshAction( m_captureProtectionMonitorAction );
    //ToolbarIcons::refreshAction( m_captureProtectionExcludeAction );
}

QBaseView* MainWindow::createViewForFile( const QString& filePath )
{
    const QString ext = QFileInfo( filePath ).suffix().toLower();

    //if( ext == "pdf" )
    //    return new QPDFView( this );

    //if( mrst::filekinds::imageExtensions().contains( ext ) )
    //    return new QImageView( this );

    return new QTextView( this );
}

void MainWindow::applyPersistedViewSettings( QBaseView* view )
{
    if( !view )
        return;

    AppSettings s;

    //if( auto* imageView = qobject_cast< QImageView* >( view ) )
    //{
    //    const int fitMode = s.value( "image/defaultFitMode", 1 ).toInt();
    //    imageView->setFitMode( static_cast< QImageView::FitMode >( qBound( 0, fitMode, 1 ) ) );
    //    const QColor drawColor( s.value( "image/drawColor", ThemeManager::instance().color( QStringLiteral( "image.draw" ) ).name() ).toString() );
    //    if( drawColor.isValid() )
    //        imageView->setPenColor( drawColor );
    //    imageView->setPenWidth( qBound( 1, s.value( "image/drawWidth", 2 ).toInt(), 20 ) );
    //}
    /* else */ if( auto* textView = qobject_cast< QTextView* >( view ) )
    {
        // 텍스트 뷰어는 자체적으로 loadPersistedEditorPreferences()를 호출하므로
        // 여기서는 글꼴과 행간 등 추가 설정만 적용
        const QString fontFamily = s.value( "textView/fontFamily", "Consolas" ).toString();
        const int fontSize = qBound( 6, s.value( "textView/fontSize", 10 ).toInt(), 72 );
        textView->setEditorFont( QFont( fontFamily, fontSize ) );
        textView->setLineSpacingScale( qBound( 1.0, s.value( "textView/lineSpacing", 1.1 ).toDouble(), 3.0 ) );
    }
}

void MainWindow::applySettingsToAllViews()
{
    if( !m_tabWidget )
        return;

    AppSettings s;

    for( int i = 0; i < m_tabWidget->count(); ++i )
    {
        auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( i ) );
        if( !view )
            continue;

        if( auto* textView = qobject_cast< QTextView* >( view ) )
        {
            const QString fontFamily = s.value( "textView/fontFamily", "Consolas" ).toString();
            const int fontSize = qBound( 6, s.value( "textView/fontSize", 10 ).toInt(), 72 );
            textView->setEditorFont( QFont( fontFamily, fontSize ) );
            textView->setLineSpacingScale( qBound( 1.0, s.value( "textView/lineSpacing", 1.1 ).toDouble(), 3.0 ) );
            textView->setTabWidth( qBound( 1, s.value( "textView/tabWidth", 4 ).toInt(), 16 ) );
            textView->setUseTabs( s.value( "textView/useTabs", true ).toBool() );
            textView->setIndentationGuidesVisible( s.value( "textView/showIndentationGuides", true ).toBool() );
            textView->setIndentGuideStyle( static_cast< ScintillaEditorSettings::IndentGuideStyle >(
                qBound( 1, s.value( "textView/indentGuideStyle", 1 ).toInt(), 3 ) ) );
            textView->setWordWrapMode( static_cast< ScintillaEditorSettings::WrapMode >(
                qBound( 0, s.value( "textView/wordWrapMode", 2 ).toInt(), 3 ) ) );
            textView->setWrapVisualFlags( s.value( "textView/wrapVisualFlags", 1 ).toInt() );
            textView->setWrapIndentMode( static_cast< ScintillaEditorSettings::WrapIndentMode >(
                qBound( 0, s.value( "textView/wrapIndentMode", 1 ).toInt(), 3 ) ) );
            textView->setWhitespaceVisible( s.value( "textView/showWhitespace", false ).toBool() );
            textView->setCodeFoldingEnabled( s.value( "textView/showCodeFolding", true ).toBool() );
            textView->setBraceHighlightEnabled( s.value( "textView/braceHighlight", true ).toBool() );
            const int fontRendering = s.value( "textView/fontRendering", 2 ).toInt();
            textView->setFontRenderingMode( static_cast< ScintillaEditorSettings::FontRenderingMode >(
                qBound( 0, fontRendering, 3 ) ) );
            const int changeHistory = s.value( "textView/changeHistoryMode", 3 ).toInt();
            textView->setChangeHistoryMode( static_cast< ScintillaEditorSettings::ChangeHistoryMode >(
                qBound( 0, changeHistory, 3 ) ) );
            textView->setHotExitEnabled( s.value( "textView/hotExitEnabled", true ).toBool() );
        }
    //    else if( auto* imageView = qobject_cast< QImageView* >( view ) )
    //    {
    //        const QColor drawColor( s.value( "image/drawColor", ThemeManager::instance().color( QStringLiteral( "image.draw" ) ).name() ).toString() );
    //        if( drawColor.isValid() )
    //            imageView->setPenColor( drawColor );
    //        imageView->setPenWidth( qBound( 1, s.value( "image/drawWidth", 2 ).toInt(), 20 ) );
    //    }
    //    // PDF 뷰 모드는 이미 열린 문서에는 적용하지 않음 (사용자가 수동 변경 가능)
    }
}

QBaseView* MainWindow::currentView() const
{
    return qobject_cast< QBaseView* >( m_tabWidget->currentWidget() );
}

int MainWindow::addViewTab( QBaseView* view )
{
    if( !view || !m_tabWidget )
        return -1;

    view->setAcceptDrops( true );

    const int idx = m_tabWidget->addTab( view, view->title() );
    m_tabWidget->setCurrentIndex( idx );
    updateTabDecoration( view );

    connect( view, &QBaseView::sigTitleChanged, this, [this, view]( const QString& title ) {
        const int i = m_tabWidget ? m_tabWidget->indexOf( view ) : -1;
        if( i >= 0 )
            updateTabDecoration( view );
        if( currentView() == view )
            updateTitle();
    } );

    connect( view, &QBaseView::sigModifiedChanged, this, [this, view]( bool ) {
        updateTabDecoration( view );
        if( currentView() == view )
            updateTitle();
    } );

    connectViewStatusSignals( view );

    if( controller_ )
    {
        if( QTextView* textView = textViewOf( view ) )
        {
            controller_->attachDocument( textView );
            controller_->setActiveDocument( textView );
        }
    }

    updateCopyActionState();
    updatePasteActionState();
    return idx;
}

void MainWindow::disconnectViewSignals( QBaseView* view )
{
    if( !view )
        return;

    if( controller_ )
    {
        if( QTextView* textView = textViewOf( view ) )
            controller_->detachDocument( textView );
    }

    QObject::disconnect( view, nullptr, this, nullptr );
}

void MainWindow::removeViewTabWithoutSignals( QBaseView* view )
{
    if( !view || !m_tabWidget )
        return;

    const int index = m_tabWidget->indexOf( view );
    if( index < 0 )
        return;

    const QSignalBlocker blocker( m_tabWidget );
    m_tabWidget->removeTab( index );
    view->hide();
    view->setParent( nullptr );
}

void MainWindow::teardownView( QBaseView* view )
{
    teardownView( view, ViewTeardownOptions{} );
}

void MainWindow::teardownView( QBaseView* view, const ViewTeardownOptions& options )
{
    if( !view )
        return;

    m_activeViewLoads.remove( view );

    if( options.disconnectSignals )
        disconnectViewSignals( view );

    if( options.blockViewSignals )
        view->blockSignals( true );

    if( options.closeFile )
        view->closeFile();

    if( options.removeTab )
        removeViewTabWithoutSignals( view );

    if( options.deleteLater )
        view->deleteLater();
}

void MainWindow::refreshCurrentViewUi()
{
    refreshLoadingIndicator();
    updateTitle();
    updateViewerToolBar();
    updateStatusBar();
    updateSaveActionState();
    updateCopyActionState();
    updatePasteActionState();
}

void MainWindow::connectViewStatusSignals( QBaseView* view )
{
    if( !view )
        return;

    if( view->property( "mv_statusSignalsConnected" ).toBool() )
        return;
    view->setProperty( "mv_statusSignalsConnected", true );

    connect( view, &QBaseView::sigLoadingStateChanged, this,
            [this, view]( bool active, const QString& message, int value, int maximum ) {
                setViewLoadingState( view, active, message, value, maximum );
            } );
    connect( view, &QObject::destroyed, this, [this, view] {
        m_activeViewLoads.remove( view );
        refreshLoadingIndicator();
    } );

    //connect( view, &QBaseView::copyAvailabilityChanged, this, [this, view]( bool ) {
    //    if( currentView() == view )
    //        updateCopyActionState();
    //} );

    //if( auto* imageView = qobject_cast< QImageView* >( view ) )
    //{
    //    connect( imageView, &QImageView::imageChanged, this, [this, imageView] {
    //        if( currentView() == imageView )
    //            updateStatusBar();
    //    } );
    //    connect( imageView, &QImageView::viewStateChanged, this, [this, imageView] {
    //        if( currentView() == imageView )
    //            updateStatusBar();
    //    } );
    //}

    if( auto* textView = qobject_cast< QTextView* >( view ) )
    {
        connect( textView, &QTextView::statusChanged, this, [this, textView] {
            if( currentView() == textView )
                updateStatusBar();
        } );
        connect( textView, &QTextView::encodingChanged, this, [this, textView] {
            if( currentView() == textView )
                updateStatusBar();
        } );
        connect( textView, &QTextView::languageChanged, this, [this, textView] {
            if( currentView() == textView )
                updateStatusBar();
        } );
    }
}

// ═══════════════════════════════════════════════════════════
// 슬롯
// ═══════════════════════════════════════════════════════════
void MainWindow::onFileNew()
{
    // 이 앱이 여는 것은 텍스트 문서뿐이다. 예전에는 텍스트/이미지 뷰어 중
    // 무엇으로 시작할지 물었지만 이미지 뷰어는 이 포팅에 없다.
    auto* view = new QTextView( this );
    applyPersistedViewSettings( view );
    applyThemeToView( view );
    addViewTab( view );
}

void MainWindow::onFileOpen()
{
    // 필터 이름만 번역하고 글롭 패턴과 ";;" 구분자는 코드가 만든다. 통째로
    // tr() 에 넣으면 번역자가 전각 괄호를 쓰거나 ";;" 를 하나로 줄이는 순간
    // 파일이 하나도 보이지 않는다.
    static const QString kAllSupported =
        QStringLiteral( "*.rst *.txt *.log *.ini *.cfg *.xml *.json *.html *.htm *.css "
                        "*.js *.ts *.cpp *.c *.h *.hpp *.py *.java *.md *.markdown" );
    static const QString kText =
        QStringLiteral( "*.txt *.log *.ini *.cfg *.xml *.json *.html *.css *.js *.cpp *.c *.h *.py" );

    const QStringList filters{
        QStringLiteral( "%1 (%2)" ).arg( tr( "모든 지원 파일" ), kAllSupported ),
        QStringLiteral( "reStructuredText (*.rst)" ),
        QStringLiteral( "%1 (%2)" ).arg( tr( "텍스트" ), kText ),
        QStringLiteral( "%1 (*.md *.markdown)" ).arg( tr( "마크다운" ) ),
        QStringLiteral( "%1 (*)" ).arg( tr( "모든 파일" ) ),
    };

    const QStringList files = QFileDialog::getOpenFileNames( this,
        tr( "파일 열기" ), {}, filters.join( QStringLiteral( ";;" ) ) );
    for( const auto& f : files )
        openFile( f );
}

void MainWindow::onWorkspaceOpen()
{
    //if( !confirmSaveAll() )
    //    return;
    const QString startDir = controller_ ? controller_->workspaceRoot() : QString{};
    const QString folder = QFileDialog::getExistingDirectory( this, tr( "워크스페이스 폴더 열기" ), startDir );
    if( !folder.isEmpty() )
        setWorkspace( folder );
}

void MainWindow::onFileSave()
{
    saveView( currentView(), false );
}

void MainWindow::onFileSaveAs()
{
    saveView( currentView(), true );
}

bool MainWindow::saveView( QBaseView* view, bool saveAs )
{
    if( !view || view->isLoading() )
        return false;

    bool started = false;
    if( auto* textView = qobject_cast< QTextView* >( view ) )
        started = saveAs ? textView->saveFileAs() : textView->saveFile( {} );
    //else if( auto* imageView = qobject_cast< QImageView* >( view ) )
    //    started = saveAs ? imageView->saveFileAs() : imageView->saveFile( {} );
    //else if( auto* pdfView = qobject_cast< QPDFView* >( view ) )
    //    started = saveAs ? pdfView->saveFileAs() : pdfView->saveFile( {} );
    else
        started = view->saveFile();

    if( started && controller_ )
    {
        // 다른 이름으로 저장이면 경로가 바뀌었을 수 있어 프로젝트를 다시 해석한다.
        if( QTextView* textView = textViewOf( view ) )
            controller_->notifyDocumentSaved( textView );
    }

    updateSaveActionState();
    return started;
}

void MainWindow::onCopy()
{
    if( auto* v = currentView() )
        v->copySelectionToClipboard();

    updateCopyActionState();
}

void MainWindow::updateSaveActionState()
{
    const bool enabled = currentView() && !currentView()->isLoading();
    if( m_saveAction )
        m_saveAction->setEnabled( enabled );
    if( m_saveAsAction )
        m_saveAsAction->setEnabled( enabled );
}

void MainWindow::onPaste()
{
    // 편집 메뉴의 붙여넣기. 예전에는 클립보드 이미지를 이미지 뷰어 탭으로 여는
    // 것이 전부였는데, 이 포팅에는 이미지 뷰어가 없어 아무 일도 하지 않았다.
    // 이제는 활성 문서에 그대로 붙여넣는다.
    if( auto* view = currentView() )
        view->pasteFromClipboard();

    updatePasteActionState();
}

void MainWindow::onCloseTab( int index )
{
    if( index < 0 || !m_tabWidget || index >= m_tabWidget->count() ) return;

    QWidget* widget = m_tabWidget->widget( index );
    if( auto* view = qobject_cast< QBaseView* >( widget ) )
    {
        if( m_activeViewLoads.contains( view ) )
        {
            cancelLoadingView( view, false );
            return;
        }
        if( view->isModified() && canCloseWithTextHotExit( view ) )
        {
            if( auto* textView = qobject_cast< QTextView* >( view ) )
                textView->flushHotExitBackup();
        }
        else if( view->isModified() )
        {
            auto btn = QMessageBox::question( this, tr( "저장 확인" ),
                tr( "변경사항이 있습니다. 저장하시겠습니까?" ),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel );
            if( btn == QMessageBox::Cancel ) return;
            if( btn == QMessageBox::Yes )
            {
                // saveAs=false. 여기서 "예"는 "저장해라"(= Ctrl+S)이지 "다른 이름으로
                // 저장해라"가 아니다. true 를 주면 saveFileAs() -> TextSaveDialog 로
                // 이어져, 경로가 이미 있는 파일에도 "다른 이름으로 저장" 대화상자가
                // 탭마다 하나씩 뜬다. 이름 없는 새 문서는 saveFile() 안에서 경로가
                // 비어 있을 때 그 대화상자로 물러서므로 그대로 동작한다.
                if( !saveView( view, false ) )
                    return;
                if( view->isLoading() )
                {
                    statusBar()->showMessage( tr( "저장이 진행 중입니다. 저장 완료 후 다시 탭을 닫아 주세요." ), 3000 );
                    refreshCurrentViewUi();
                    return;
                }
            }
        }
        teardownView( view );
    }
    else
    {
        const QSignalBlocker blocker( m_tabWidget );
        m_tabWidget->removeTab( index );
        widget->deleteLater();
    }

    refreshCurrentViewUi();
}

void MainWindow::closeEvent( QCloseEvent* event )
{
    mrst::traceP( "close.enter" );

    // 종료 의사를 **저장 확인보다 먼저** 알린다. 아래 루프의 setCurrentIndex() 가
    // onTabChanged 를 일으켜 프리뷰 빌드와 LSP 프로세스를 새로 띄우고, 저장이
    // 성공하면 notifyDocumentSaved() 가 용어집 전량 재스캔까지 던진다.
    // 컨트롤러의 shuttingDown_ 가드는 이미 그 경로를 전부 막고 있는데,
    // 지금까지는 shutdown() 에서야 켜져서(=아래 shutdownUi 이후) 소용이 없었다.
    if( controller_ )
        controller_->beginShutdown();

    // 워커 스레드에도 알린다. 용어집(최대 2000문서)·개요(최대 500문서)·워크스페이스
    // 스캔·서명 검증(90MB 해시)·업데이트 잔재 삭제(150~400MB)는 전부 끝까지 도는
    // 동안 프로세스를 붙잡는다. 결과는 어차피 버려지므로 일찍 멈추는 편이 낫다.
    mrst::requestShutdown();

    // 아직 시작하지 않은 배경 작업은 통째로 버린다. 저장과 hot-exit 스냅샷은
    // persistencePool() 로 분리되어 있어 영향받지 않는다 — 그것을 버리면
    // 데이터가 사라진다.
    QThreadPool::globalInstance()->clear();

    // 사용자가 취소를 눌러 종료가 되돌아가는 경로가 둘 있다. 어느 쪽이든
    // 표시를 되돌리고 활성 문서를 다시 반영해야 프리뷰/LSP 가 되살아난다.
    const auto abortShutdown = [this] {
        // 워커 표시를 먼저 지운다. 남겨 두면 이후 세션 동안 개요·용어집·스캔이
        // 전부 즉시 포기한다.
        mrst::cancelShutdownRequest();
        if( controller_ == nullptr )
            return;
        controller_->endShutdown();
        controller_->setActiveDocument( textViewOf( currentView() ) );
    };

    if( m_tabWidget )
    {
        for( int i = 0; i < m_tabWidget->count(); ++i )
        {
            QWidget* widget = m_tabWidget->widget( i );
            if( auto* view = qobject_cast< QBaseView* >( widget ) )
            {
                if( view->isModified() && canCloseWithTextHotExit( view ) )
                {
                    if( auto* textView = qobject_cast< QTextView* >( view ) )
                        textView->flushHotExitBackup();
                }
                else if( view->isModified() )
                {
                    m_tabWidget->setCurrentIndex( i );
                    auto btn = QMessageBox::question( this, tr( "저장 확인" ),
                        tr( "변경사항이 있습니다. 저장하시겠습니까?\n%1" ).arg( QFileInfo( view->currentFilePath() ).fileName() ),
                        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel );
                    if( btn == QMessageBox::Cancel )
                    {
                        // 종료를 되돌리면 설치 예약도 함께 취소해야 한다. 남겨 두면
                        // 다음 종료 때 사용자가 요청하지 않은 설치가 시작된다.
                        pendingInstall_ = false;
                        abortShutdown();
                        event->ignore();
                        return;
                    }
                    if( btn == QMessageBox::Yes )
                    {
                        // saveAs=false — onCloseTab 과 같은 이유다. 종료 확인에서
                        // "예"는 "저장해라"이고, true 를 주면 수정된 탭마다
                        // "다른 이름으로 저장" 대화상자가 하나씩 떠서 종료가 멈춘다.
                        if( !saveView( view, false ) )
                        {
                            pendingInstall_ = false;
                            abortShutdown();
                            event->ignore();
                            return;
                        }
                    }
                }
            }
        }
    }
    mrst::traceP( "close.prompts-done" );

    saveWorkspaceSessionNow();
    mrst::traceP( "close.session-saved" );

    shutdownUi();
    mrst::traceP( "close.ui-shutdown" );

    // 여기까지 왔으면 저장 확인을 모두 통과했고, shutdownUi() 가 LSP/프리뷰/
    // WebEngine 자식 프로세스를 정리했다(Job Object 덕에 손자 sphinx_agent 까지).
    // 즉 교체 대상 파일에 우리가 건 잠금이 남아 있지 않다.
    if( pendingInstall_ && updateService_ != nullptr )
    {
        if( !updateService_->launchInstaller() )
        {
            // staging 은 그대로 두므로 다음 실행에서 다시 설치할 수 있다.
            appendLog( tr( "업데이터를 시작하지 못했습니다. 다음 실행 때 다시 시도합니다." ) );
        }
    }

    mrst::traceP( "close.leave" );
    QMainWindow::closeEvent( event );
}

void MainWindow::onTabChanged( int /*index*/ )
{
    if( controller_ )
        controller_->setActiveDocument( textViewOf( currentView() ) );

    updateTitle();
    updateViewerToolBar();
    updateStatusBar();
    refreshLoadingIndicator();
    updateSaveActionState();
    updateCopyActionState();
    updatePasteActionState();
}

void MainWindow::onThemeToggle()
{
    auto& mgr = ThemeManager::instance();
    mgr.setTheme( mgr.currentTheme() == ThemeManager::Light
                     ? ThemeManager::Dark : ThemeManager::Light );
}

void MainWindow::appendLog( const QString& text )
{
    if( Ui.logView == nullptr )
        return;

    const auto trimmed = text.trimmed();
    if( trimmed.isEmpty() == true )
        return;

    Ui.logView->appendPlainText( trimmed );

    // MRST_LOG_FILE 이 지정되면 로그 창 내용을 파일로도 남긴다.
    // (MV_TEXT_LEXER_TRACE_FILE 과 같은 방식. GUI 없이 동작을 확인할 때 쓴다.)
    static const QString logFilePath =
        QString::fromLocal8Bit( qgetenv( "MRST_LOG_FILE" ) ).trimmed();
    if( logFilePath.isEmpty() )
        return;

    QFile logFile( logFilePath );
    if( logFile.open( QIODevice::Append | QIODevice::Text ) )
    {
        QTextStream stream( &logFile );
        stream.setEncoding( QStringConverter::Utf8 );
        stream << trimmed << Qt::endl;
    }
}

void MainWindow::onSettings()
{
    QSettingsDialog dlg( this );
    connect( &dlg, &QSettingsDialog::settingsApplied, this, [this] {
        // 단축키 즉시 적용
        const auto shortcuts = QSettingsDialog::LoadShortcutsFromSettings();
        QSettingsDialog::ApplyShortcutsToActions( shortcuts, this );
        // 열려있는 뷰어에 변경된 설정 적용
        applySettingsToAllViews();
        // 스캐너 제외 목록 / 최대 Esbonio 프로세스 수 등도 즉시 반영한다.
        if( controller_ != nullptr )
            controller_->reloadSettings();
        if( updateService_ != nullptr )
            updateService_->reloadSettings();
    } );
    if( updateService_ != nullptr )
    {
        // 대화상자는 UpdateService 를 소유하지 않는다. 요청만 넘기고, 상태가
        // 바뀌면 "마지막 확인" 라벨을 다시 읽게 한다.
        connect( &dlg, &QSettingsDialog::updateCheckRequested, this,
                [this] { updateService_->checkAsync( /*userInitiated=*/true ); } );
        connect( updateService_, &mrst::UpdateService::stateChanged, &dlg,
                &QSettingsDialog::refreshUpdateStatus );
    }
    dlg.exec();
}

void MainWindow::onAbout()
{
    QAboutDialog dlg( updateService_, this );
    if( updateService_ != nullptr )
    {
        // 설정 대화상자와 같은 규칙이다 — 대화상자는 UpdateService 를 읽기만
        // 하고, 점검을 시작하는 것은 그것을 가진 이쪽이다. 그래야 새 버전을
        // 찾았을 때 알림 바까지 평소 경로대로 뜬다.
        connect( &dlg, &QAboutDialog::updateCheckRequested, this,
                [this] { updateService_->checkAsync( /*userInitiated=*/true ); } );
    }
    dlg.exec();
}

void MainWindow::updateTitle()
{
    auto* v = currentView();
    if( v )
        //: 창 제목. %1 은 현재 문서 이름이다. 제품 이름은 옮기지 않는다.
        setWindowTitle( tr( "MultiRoot reST Editor — %1" ).arg( v->title() ) );
    else
        setWindowTitle( tr( "MultiRoot reST Editor" ) );
}

void MainWindow::updateTabDecoration( QBaseView* view )
{
    if( !view || !m_tabWidget )
        return;

    const int index = m_tabWidget->indexOf( view );
    if( index < 0 )
        return;

    QString tabTitle = view->title();
    if( view->isModified() )
        tabTitle.prepend( QStringLiteral( "● " ) );
    m_tabWidget->setTabText( index, tabTitle );
    m_tabWidget->setTabIcon( index, tabIconForView( view ) );
}

void MainWindow::updateStatusBar()
{
    // 이전의 임시 메시지(showMessage)가 permanent widget을 가리지 않도록 제거
    statusBar()->clearMessage();

    auto* v = currentView();
    if( !v ) { m_statusLabel->clear(); return; }

    QString info;
    //if( auto* iv = qobject_cast< QImageView* >( v ) )
    //{
    //    const QSize size = iv->originalImageSize();
    //    const QPointF dpi = iv->imageDpi();
    //    const QString viewModeText = iv->fitMode() == QImageView::OriginalSize
    //        ? tr( "표시 %1%" ).arg( static_cast< int >( iv->displayScalePercent() ) )
    //        : tr( "%1 (%2%)" ).arg( imageFitModeText( iv->fitMode() ) )
    //        .arg( static_cast< int >( iv->displayScalePercent() ) );
    //    const QString metaText = dpi.x() > 0.0 && dpi.y() > 0.0
    //        ? tr( "%1 | %2 | %3×%4 DPI" )
    //        .arg( iv->imageFormatName(), iv->imageColorDescription() )
    //        .arg( qRound( dpi.x() ) )
    //        .arg( qRound( dpi.y() ) )
    //        : tr( "%1 | %2" ).arg( iv->imageFormatName(), iv->imageColorDescription() );
    //    const int imageCount = iv->imageCountInDirectory();
    //    if( imageCount > 0 )
    //    {
    //        info = tr( "폴더 %1/%2 | 원본 %3 × %4 | %5 | %6" )
    //            .arg( iv->imageIndexInDirectory() + 1 )
    //            .arg( imageCount )
    //            .arg( size.width() )
    //            .arg( size.height() )
    //            .arg( metaText )
    //            .arg( viewModeText );
    //    }
    //    else
    //    {
    //        info = tr( "원본 %1 × %2 | %3 | %4" )
    //            .arg( size.width() )
    //            .arg( size.height() )
    //            .arg( metaText )
    //            .arg( viewModeText );
    //    }
    //}
    /*else*/ if( auto* tv = qobject_cast< QTextView* >( v ) )
    {
        const QString language = tv->currentLanguage().isEmpty() ? tr( "None" ) : tv->currentLanguage();
        const int selectionLength = tv->selectedCharacterCount();
        const QString modeText = tv->isReadOnly() ? tr( "읽기 전용" ) : tr( "편집 가능" );
        const QString loadModeText = tv->contentLoadModeText();
        const QString whitespaceText = tv->isWhitespaceVisible()
            ? tr( "제어문자 표시" )
            : tr( "제어문자 숨김" );
        const QString textStats = selectionLength > 0
            ? tr( "문자 %1 | 선택 %2" ).arg( tv->characterCount() ).arg( selectionLength )
            : tr( "문자 %1" ).arg( tv->characterCount() );
        const QString eolText = [&] {
            switch( tv->detectedLineEnding() )
            {
                case QTextView::LF: return QStringLiteral( "LF" );
                case QTextView::CR: return QStringLiteral( "CR" );
                default: return QStringLiteral( "CRLF" );
            }
            }( );
        const QString indentText = tv->useTabs()
            ? tr( "탭 %1" ).arg( tv->tabWidth() )
            : tr( "공백 %1" ).arg( tv->tabWidth() );
        info = tr( "줄 %1/%2 | 열 %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10 | %11" )
            .arg( tv->currentLine() )
            .arg( tv->lineCount() )
            .arg( tv->currentColumn() )
            .arg( modeText )
            .arg( loadModeText )
            .arg( textStats )
            .arg( indentText )
            .arg( whitespaceText )
            .arg( eolText )
            .arg( tv->currentEncodingDisplayName() )
            .arg( language );
    }
    m_statusLabel->setText( info );
}

void MainWindow::updateCopyActionState()
{
    if( m_copyAction )
        m_copyAction->setEnabled( currentView() && currentView()->canCopyToClipboard() );
}

QBaseView* MainWindow::preferredLoadingView() const
{
    QBaseView* displayView = currentView();
    if( displayView && m_activeViewLoads.contains( displayView ) )
        return displayView;

    for( auto it = m_activeViewLoads.cbegin(); it != m_activeViewLoads.cend(); ++it )
    {
        if( it.key() )
            return it.key();
    }

    return nullptr;
}

QIcon MainWindow::tabIconForView( QBaseView* view ) const
{
    if( !view )
        return {};

    if( !m_activeViewLoads.contains( view ) )
        return {};

    return {};
    //return loadingTabIcon( baseIcon, palette(), m_loadingAnimationFrame );
}

void MainWindow::advanceLoadingAnimation()
{
    if( m_activeViewLoads.isEmpty() )
    {
        if( m_loadingAnimationTimer )
            m_loadingAnimationTimer->stop();
        return;
    }

    m_loadingAnimationFrame = ( m_loadingAnimationFrame + 1 ) % kLoadingAnimationFrameCount;
    for( auto it = m_activeViewLoads.cbegin(); it != m_activeViewLoads.cend(); ++it )
    {
        if( it.key() )
            updateTabDecoration( it.key() );
    }
}

void MainWindow::onCancelLoading()
{
    cancelLoadingView( preferredLoadingView() );
}

void MainWindow::cancelLoadingView( QBaseView* view, bool showStatusMessage )
{
    if( !view || !m_activeViewLoads.contains( view ) )
        return;

    const QString cancelledTitle = view->title();

    teardownView( view );
    refreshCurrentViewUi();

    if( showStatusMessage )
    {
        statusBar()->showMessage(
            tr( "\"%1\" 파일 열기를 취소했습니다." ).arg( cancelledTitle.isEmpty() ? tr( "현재 탭" ) : cancelledTitle ),
            2500 );
    }
}

void MainWindow::setViewLoadingState( QBaseView* view,
                                     bool active,
                                     const QString& message,
                                     int value,
                                     int maximum )
{
    if( !m_loadingLabel || !m_loadingProgressBar )
        return;

    if( !view )
        return;

    const bool wasActive = m_activeViewLoads.contains( view );

    if( active )
        m_activeViewLoads.insert( view, { message, value, maximum } );
    else
        m_activeViewLoads.remove( view );

    updateTabDecoration( view );
    refreshLoadingIndicator();

    // 로딩 상태가 전환된 현재 뷰의 도구모음 업데이트:
    // 이미 도구모음이 존재하면 재생성하지 않음 (파일 저장 시 깜빡임 방지)
    // 도구모음이 없을 때만 갱신 (파일 열기 완료 후 도구모음 최초 생성)
    if( currentView() == view && wasActive != active && !m_viewerToolBar )
        updateViewerToolBar();

    if( !active )
    {
        if( !message.isEmpty() && m_activeViewLoads.isEmpty() )
            statusBar()->showMessage( message, 2000 );
        else if( m_activeViewLoads.isEmpty() )
            updateStatusBar();
    }

    updateSaveActionState();
}

void MainWindow::refreshLoadingIndicator()
{
    if( !m_loadingLabel || !m_loadingProgressBar )
        return;

    QBaseView* displayView = preferredLoadingView();

    if( m_loadingAnimationTimer )
    {
        if( m_activeViewLoads.isEmpty() )
            m_loadingAnimationTimer->stop();
        else if( !m_loadingAnimationTimer->isActive() )
            m_loadingAnimationTimer->start();
    }

    if( !displayView || !m_activeViewLoads.contains( displayView ) )
    {
        m_loadingLabel->clear();
        m_loadingLabel->setVisible( false );
        m_loadingProgressBar->setVisible( false );
        m_loadingProgressBar->reset();
        m_loadingProgressBar->setFormat( QStringLiteral( "%p%" ) );
        if( m_loadingCancelButton )
        {
            m_loadingCancelButton->setVisible( false );
            m_loadingCancelButton->setToolTip( {} );
        }
        // 로딩 중 표시한 임시 메시지 제거 → permanent 위젯이 보이도록
        statusBar()->clearMessage();
        return;
    }

    const ViewLoadingState state = m_activeViewLoads.value( displayView );
    QString effectiveMessage = state.message.isEmpty()
        ? tr( "파일 여는 중..." )
        : state.message;
    if( !displayView->title().isEmpty() )
        effectiveMessage = tr( "%1 — %2" ).arg( displayView->title(), effectiveMessage );

    if( state.maximum > 0 )
    {
        const int boundedValue = qBound( 0, state.value, state.maximum );
        const int percent = qRound( ( static_cast< double >( boundedValue ) * 100.0 ) / state.maximum );
        m_loadingLabel->setText( tr( "%1 (%2%)" ).arg( effectiveMessage ).arg( percent ) );
        m_loadingProgressBar->setRange( 0, state.maximum );
        m_loadingProgressBar->setValue( boundedValue );
        m_loadingProgressBar->setFormat( QStringLiteral( "%p%" ) );
    }
    else
    {
        m_loadingLabel->setText( effectiveMessage );
        m_loadingProgressBar->setRange( 0, 0 );
        m_loadingProgressBar->setFormat( tr( "작업 중" ) );
    }

    m_loadingLabel->setVisible( true );
    m_loadingProgressBar->setVisible( true );
    if( m_loadingCancelButton )
    {
        m_loadingCancelButton->setVisible( displayView->canCancelLoading() );
        m_loadingCancelButton->setToolTip( tr( "\"%1\" 파일 열기를 취소합니다." ).arg( displayView->title() ) );
    }
    statusBar()->showMessage( m_loadingLabel->text() );
}

void MainWindow::updatePasteActionState()
{
    if( m_pasteAction )
        m_pasteAction->setEnabled( currentView() && currentView()->canPasteFromClipboard() );
}

// ═══════════════════════════════════════════════════════════
// 드래그 앤 드롭
// ═══════════════════════════════════════════════════════════
bool MainWindow::eventFilter( QObject* watched, QEvent* event )
{
    if( !event )
        return QMainWindow::eventFilter( watched, event );

    const QEvent::Type type = event->type();

    // 첫 페인트가 곧 "사용자가 창을 본 시각" 이다. show() 는 페인트를 예약만 하고
    // exec() 전에는 아무것도 그려지지 않으므로, 그 전에 시작한 무거운 일은 전부
    // 창이 비어 있는 시간이 된다. 이 이벤트는 아직 처리되지 않았으니 실제로
    // 그려진 뒤로 미룬다(0ms 큐잉).
    //
    // window() == this 로 좁히는 이유: QMainWindow 자신은 자식에 완전히 덮이면
    // Paint 를 못 받을 수 있다. 어느 자식이 먼저 그려지든 잡아야 한다.
    if( !firstPaintSeen_ && type == QEvent::Paint )
    {
        if( auto* painted = qobject_cast< QWidget* >( watched );
            painted != nullptr && painted->window() == this )
        {
            firstPaintSeen_ = true;
            mrst::traceP( "firstpaint" );
            QTimer::singleShot( 0, this, &MainWindow::advanceStartupPhase );
        }
    }

    if( type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::Drop )
        return QMainWindow::eventFilter( watched, event );

    auto* widget = qobject_cast< QWidget* >( watched );
    if( !widget || ( widget != this && !isAncestorOf( widget ) ) )
        return QMainWindow::eventFilter( watched, event );

    if( type == QEvent::DragEnter )
    {
        auto* dragEvent = dynamic_cast< QDragEnterEvent* >( event );
        if( !dragEvent )
            return QMainWindow::eventFilter( watched, event );
        if( hasDroppedPaths( dragEvent->mimeData() ) )
        {
            dragEvent->acceptProposedAction();
            return true;
        }
    }
    else if( type == QEvent::DragMove )
    {
        auto* dragEvent = dynamic_cast< QDragMoveEvent* >( event );
        if( !dragEvent )
            return QMainWindow::eventFilter( watched, event );
        if( hasDroppedPaths( dragEvent->mimeData() ) )
        {
            dragEvent->acceptProposedAction();
            return true;
        }
    }
    else if( type == QEvent::Drop )
    {
        auto* drop = dynamic_cast< QDropEvent* >( event );
        if( !drop )
            return QMainWindow::eventFilter( watched, event );
        if( handleDropEvent( drop ) )
            return true;
    }

    return QMainWindow::eventFilter( watched, event );
}

void MainWindow::dragEnterEvent( QDragEnterEvent* event )
{
    if( hasDroppedPaths( event->mimeData() ) )
        event->acceptProposedAction();
    else
        event->ignore();
}

void MainWindow::dragMoveEvent( QDragMoveEvent* event )
{
    if( hasDroppedPaths( event->mimeData() ) )
        event->acceptProposedAction();
    else
        event->ignore();
}

void MainWindow::dropEvent( QDropEvent* event )
{
    if( !handleDropEvent( event ) )
        event->ignore();
}

QStringList MainWindow::droppedPathsFromMimeData( const QMimeData* mimeData ) const
{
    QStringList paths;
    QSet<QString> seen;

    const auto addPath = [&paths, &seen]( const QString& candidate ) {
        const QString path = QDir::fromNativeSeparators( stripOptionalQuotes( candidate ) );
        if( path.isEmpty() )
            return;

        const QString key = droppedPathKey( path );
        if( seen.contains( key ) )
            return;

        seen.insert( key );
        paths.append( path );
        };

    if( !mimeData )
        return paths;

    if( mimeData->hasUrls() )
    {
        for( const QUrl& url : mimeData->urls() )
            addPath( pathFromDroppedUrl( url ) );
    }

    if( mimeData->hasText() )
    {
        QString text = mimeData->text();
        text.replace( QLatin1Char( '\r' ), QLatin1Char( '\n' ) );
        const QStringList lines = text.split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );
        for( const QString& line : lines )
            addPath( pathFromDroppedTextLine( line ) );
    }

    return paths;
}

bool MainWindow::hasDroppedPaths( const QMimeData* mimeData ) const
{
    return !droppedPathsFromMimeData( mimeData ).isEmpty();
}

bool MainWindow::handleDropEvent( QDropEvent* event )
{
    if( !event )
        return false;

    const QStringList paths = droppedPathsFromMimeData( event->mimeData() );
    if( paths.isEmpty() )
        return false;

    event->acceptProposedAction();
    openDroppedPaths( paths );
    return true;
}

void MainWindow::openDroppedPaths( const QStringList& paths )
{
    int openedRequests = 0;
    for( const QString& path : paths )
    {
        const QFileInfo info( path );
        if( info.isDir() )
        {
            openDroppedDirectory( path );
            ++openedRequests;
            continue;
        }

        openFile( path );
        ++openedRequests;
    }

    if( openedRequests > 0 )
        statusBar()->showMessage( tr( "드롭한 항목 %n개를 처리했습니다.", nullptr, openedRequests ), 2500 );
}

void MainWindow::openDroppedDirectory( const QString& dirPath )
{
    // 예전에는 폴더 안의 첫 이미지를 먼저 찾아 열었다. 이미지 뷰어가 없는 지금은
    // 그 파일이 텍스트로 열려 깨져 보이기만 하므로 그냥 직계 파일들을 연다.
    QDir dir( dirPath );
    const QFileInfoList entries = dir.entryInfoList( QDir::Files, QDir::Name | QDir::IgnoreCase );
    if( entries.isEmpty() )
    {
        statusBar()->showMessage( tr( "디렉토리에 열 수 있는 직계 파일이 없습니다: %1" ).arg( QDir::toNativeSeparators( dirPath ) ), 3500 );
        return;
    }

    for( const QFileInfo& entry : entries )
        openFile( entry.absoluteFilePath() );
}


bool MainWindow::shouldConfirmBinaryTextOpen( const QString& filePath ) const
{
    const QFileInfo info( filePath );
    return info.isFile()
        && fileWouldOpenAsText( filePath )
        && isProbablyBinaryFile( filePath );
}

bool MainWindow::confirmOpenBinaryTextFile( const QString& filePath ) const
{
    QMessageBox box( QMessageBox::Warning,
                    tr( "이진 파일 열기 확인" ),
                    tr( "이 파일은 이진 데이터로 보입니다." ),
                    QMessageBox::NoButton,
                    const_cast< MainWindow* >( this ) );
    box.setInformativeText( tr( "텍스트 뷰어로 열면 내용이 깨져 보이거나 처리 시간이 오래 걸릴 수 있습니다.\n그래도 텍스트로 여시겠습니까?\n\n%1" )
                           .arg( QDir::toNativeSeparators( filePath ) ) );

    auto* openButton = box.addButton( tr( "텍스트로 열기" ), QMessageBox::AcceptRole );
    auto* cancelButton = box.addButton( QMessageBox::Cancel );
    box.setDefaultButton( cancelButton );
    box.setEscapeButton( cancelButton );
    box.exec();

    return box.clickedButton() == openButton;
}

// ═══════════════════════════════════════════════════════════
// 최근 파일
// ═══════════════════════════════════════════════════════════
void MainWindow::addRecentFile( const QString& filePath )
{
    m_recentFiles.removeAll( filePath );
    m_recentFiles.prepend( filePath );
    while( m_recentFiles.size() > MaxRecentFiles )
        m_recentFiles.removeLast();

    AppSettings settings;
    settings.setValue( "recentFiles", m_recentFiles );
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    if( !m_recentMenu ) return;
    m_recentMenu->clear();
    for( const auto& f : m_recentFiles )
    {
        m_recentMenu->addAction( QFileInfo( f ).fileName(), this, [this, f] { openFile( f ); } );
    }
    if( m_recentFiles.isEmpty() )
        m_recentMenu->addAction( tr( "(없음)" ) )->setEnabled( false );
}

void MainWindow::shutdownUi()
{
    if( m_shuttingDown ) return;
    m_shuttingDown = true;
    mrst::traceP( "shutdownUi.enter" );

    // LSP/프리뷰 프로세스는 위젯 파괴보다 먼저 정리해야 고아 프로세스가 남지 않는다.
    if( controller_ )
        controller_->shutdown();
    mrst::traceP( "shutdownUi.controller-shutdown" );

    // 진행 중인 업데이트 확인/내려받기를 끊는다. 다만 설치가 예약돼 있으면
    // 건드리지 않는다 — launchInstaller() 가 ReadyToInstall 상태를 요구한다.
    if( updateService_ != nullptr && !pendingInstall_ )
        updateService_->cancel();

    // 워크스페이스 전체를 감시하던 파일시스템 모델을 놓아 준다.
    // QFileSystemModel 은 내부에 수집 스레드를 두고 있어, 놓지 않으면 종료
    // 직전까지 배경 I/O 가 계속 돈다.
    if( treLeftFolderTreeModel_ != nullptr )
    {
        if( Ui.treLeftSideFolterTree != nullptr )
            Ui.treLeftSideFolterTree->setModel( nullptr );
        treLeftFolderTreeModel_->setRootPath( QString{} );
    }

    // 프리뷰 정리. 진행 중인 로드를 끊는 것이 핵심이다 — 이 저장소의 Breathe
    // 페이지는 하나가 6~22MB 라, 로딩 중에 닫으면 그 파싱이 종료를 붙잡는다.
    //
    // setHtml({}) 은 하지 않는다: 비동기 내비게이션이라 종료 스택 안에서
    // 완료되지 않고 취소 대상만 늘어난다. 뷰를 delete 하지도 않는다 —
    // 폼(mainWindow.ui)의 자식이라 이중 delete 가 된다.
    if( Ui.webEngineView != nullptr )
    {
        Ui.webEngineView->stop();
        if( QWebEnginePage* page = Ui.webEngineView->page() )
        {
            // PreviewBridge 는 controller_ 의 자식이라 방금 사라졌다.
            // 채널을 떼지 않으면 페이지가 죽은 객체를 가리킨다.
            page->setWebChannel( nullptr );
            page->scripts().clear();
        }
        Ui.webEngineView->hide();
    }
    mrst::traceP( "shutdownUi.web-cleared" );

    qDebug().noquote() << "[MainWindow] shutdownUi begin" << this
        << "tabCount=" << ( m_tabWidget ? m_tabWidget->count() : -1 );

    setUpdatesEnabled( false );
    if( m_centralContainer )
        m_centralContainer->setEnabled( false );

    if( m_loadingAnimationTimer )
        m_loadingAnimationTimer->stop();
    m_activeViewLoads.clear();

    if( m_tabWidget )
        m_tabWidget->blockSignals( true );
    if( m_tabWidget )
        m_tabWidget->setVisible( false );

    if( m_viewerToolBar )
    {
        qDebug().noquote() << "[MainWindow] destroy viewer toolbar" << m_viewerToolBar.data();
        destroyViewerToolBar( m_viewerToolBarHost, m_viewerToolBar );
    }

    purgeStaleViewerToolBars( m_viewerToolBarHost );

    if( !m_tabWidget ) return;

    QVector<QWidget*> widgetsToClose;
    widgetsToClose.reserve( m_tabWidget->count() );
    while( m_tabWidget->count() > 0 )
    {
        QWidget* widget = m_tabWidget->widget( m_tabWidget->count() - 1 );
        qDebug().noquote() << "[MainWindow] remove tab widget=" << widget
            << "index=" << ( m_tabWidget->count() - 1 );
        m_tabWidget->removeTab( m_tabWidget->count() - 1 );
        if( widget )
        {
            widget->hide();
            widget->setParent( nullptr );
            widgetsToClose.append( widget );
        }
    }

    for( QWidget* widget : widgetsToClose )
    {
        if( !widget )
            continue;

        qDebug().noquote() << "[MainWindow] teardown widget begin" << widget
            << "class=" << widget->metaObject()->className();

        widget->blockSignals( true );
        widget->hide();

        if( auto* view = qobject_cast< QBaseView* >( widget ) )
        {
            qDebug().noquote() << "[MainWindow] closing view" << view
                << "file=" << view->currentFilePath();
            ViewTeardownOptions teardownOptions;
            teardownOptions.disconnectSignals = true;
            teardownOptions.blockViewSignals = true;
            teardownOptions.closeFile = true;
            teardownOptions.removeTab = false;
            teardownOptions.deleteLater = false;
            teardownView( view, teardownOptions );
            delete view;
            continue;
        }

        delete widget;
    }
    mrst::traceP( "shutdownUi.tabs-torn-down" );

    qDebug().noquote() << "[MainWindow] shutdownUi end" << this;
}

///////////////////////////////////////////////////////////////////////////
/// Esbonio / Sphinx

void MainWindow::setWorkspace( const QString& Folder )
{
    const QString workspaceRoot = QFileInfo( Folder ).absoluteFilePath();

    // 워크스페이스를 옮기기 전에 지금 것의 세션을 남긴다.
    if( !workspaceRoot_.isEmpty() && workspaceRoot_ != workspaceRoot )
        saveWorkspaceSessionNow();
    workspaceRoot_ = workspaceRoot;
    AppSettings().setValue( QStringLiteral( "workspace/lastRoot" ), workspaceRoot );
    //SettingsStore::addRecent( &appState_.recentFolders, workspaceRoot_ );
    //if( workspaceSearch_ != nullptr )
    //{
    //    workspaceSearch_->setWorkspaceRoot( workspaceRoot_ );
    //}
    //if( treLeftFolderTreeModel_ == nullptr )
    //{
    //    treLeftFolderTreeModel_ = new QFileSystemModel( treeView_ );
    //    treLeftFolderTreeModel_->setReadOnly( false );
    //    treeView_->setModel( treLeftFolderTreeModel_ );
    //    connect( treeView_, &QTreeView::doubleClicked, this, [this]( const QModelIndex& index ) {
    //        if( treLeftFolderTreeModel_ == nullptr )
    //        {
    //            return;
    //        }
    //        const QString path = treLeftFolderTreeModel_->filePath( index );
    //        if( QFileInfo( path ).isFile() )
    //        {
    //            loadFile( path );
    //        }
    //    } );
    //}
    treLeftFolderTreeModel_->setRootPath( workspaceRoot );
    Ui.treLeftSideFolterTree->setRootIndex( treLeftFolderTreeModel_->index( workspaceRoot ) );
    for( int column = 1; column < treLeftFolderTreeModel_->columnCount(); ++column )
        Ui.treLeftSideFolterTree->hideColumn( column );

    // 스캔은 컨트롤러가 백그라운드로 수행하고 결과를 로그/시그널로 알려준다.
    if( controller_ )
        controller_->setWorkspaceRoot( workspaceRoot );
}

void MainWindow::refreshProjectList()
{
    if( controller_ )
        controller_->rescanProjects();
}

QTextView* MainWindow::textViewOf( QBaseView* view ) const
{
    return qobject_cast< QTextView* >( view );
}

void MainWindow::setupDiagnosticsTable()
{
    QTableWidget* table = Ui.tblDiagnostics;
    if( table == nullptr || controller_ == nullptr )
        return;

    table->setColumnCount( 5 );
    table->setHorizontalHeaderLabels( { tr( "심각도" ), tr( "파일" ), tr( "줄" ),
                                       tr( "메시지" ), tr( "출처" ) } );
    table->setEditTriggers( QAbstractItemView::NoEditTriggers );
    table->setSelectionBehavior( QAbstractItemView::SelectRows );
    table->setSelectionMode( QAbstractItemView::SingleSelection );
    table->verticalHeader()->setVisible( false );
    table->horizontalHeader()->setStretchLastSection( false );
    table->horizontalHeader()->setSectionResizeMode( 3, QHeaderView::Stretch );   // 메시지
    table->setColumnWidth( 0, 70 );
    table->setColumnWidth( 1, 180 );
    table->setColumnWidth( 2, 50 );
    table->setColumnWidth( 4, 100 );

    connect( table, &QTableWidget::itemDoubleClicked, this, [this]( QTableWidgetItem* item ) {
        if( item == nullptr )
            return;

        // 경로/줄은 0번 열 아이템에 통째로 붙여 둔다 (열마다 중복 저장하지 않도록).
        QTableWidgetItem* anchor = Ui.tblDiagnostics->item( item->row(), 0 );
        if( anchor == nullptr )
            return;

        const QString path = anchor->data( Qt::UserRole ).toString();
        const int line = anchor->data( Qt::UserRole + 1 ).toInt();
        if( path.isEmpty() )
            return;

        openFile( path );
        if( QTextView* view = textViewOf( currentView() ) )
            view->goToPosition( line, 1 );
    } );

    if( mrst::DiagnosticsStore* store = controller_->diagnostics() )
    {
        connect( store, &mrst::DiagnosticsStore::changed, this, &MainWindow::refreshDiagnosticsTable );
    }
}

// ═══════════════════════════════════════════════════════════
// 개요 트리
// ═══════════════════════════════════════════════════════════
namespace {

/// 개요 항목에 붙이는 (경로, 줄) 데이터 역할.
constexpr int kOutlinePathRole = Qt::UserRole;
constexpr int kOutlineLineRole = Qt::UserRole + 1;
/// 개요 트리가 "지금 안내 문구를 보여 주는 중" 임을 표시한다. 언어가 바뀌면
/// 그 문구만 다시 칠해야 하는데, 표시된 텍스트로는 어느 안내인지 알 수 없다
/// (번역되어 있으므로). 그래서 번역하지 않는 식별자를 함께 심는다.
constexpr int kOutlinePlaceholderRole = Qt::UserRole + 2;

QString outlineItemLabel( const mrst::OutlineSymbol& symbol )
{
    return symbol.detail.isEmpty()
               ? QStringLiteral( "%1  (%2)" ).arg( symbol.name ).arg( symbol.line )
               : QStringLiteral( "%1 — %2  (%3)" ).arg( symbol.name, symbol.detail ).arg( symbol.line );
}

void addOutlineSymbols( QTreeWidgetItem* parent, QTreeWidget* tree,
                        const QVector< mrst::OutlineSymbol >& symbols )
{
    for( const mrst::OutlineSymbol& symbol : symbols )
    {
        auto* item = new QTreeWidgetItem( QStringList{ outlineItemLabel( symbol ) } );
        if( !symbol.path.isEmpty() )
        {
            item->setData( 0, kOutlinePathRole, symbol.path );
            item->setData( 0, kOutlineLineRole, symbol.line );
        }
        if( parent != nullptr )
            parent->addChild( item );
        else
            tree->addTopLevelItem( item );

        addOutlineSymbols( item, tree, symbol.children );
    }
}

/// id 는 번역하지 않는 식별자다. 비워 두면(컨트롤러가 준 사유 문구처럼) 언어가
/// 바뀌어도 다시 칠하지 않는다 — 무슨 말이었는지 되살릴 방법이 없기 때문이다.
void setOutlinePlaceholder( QTreeWidget* tree, const QString& text, const char* id = nullptr )
{
    if( tree == nullptr )
        return;
    tree->clear();
    auto* item = new QTreeWidgetItem( QStringList{ text } );
    if( id != nullptr )
        item->setData( 0, kOutlinePlaceholderRole, QString::fromLatin1( id ) );
    tree->addTopLevelItem( item );
}

}  // namespace

void MainWindow::setupOutlineTrees()
{
    if( controller_ == nullptr )
        return;

    for( QTreeWidget* tree : { Ui.treOutlineDocument, Ui.treOutlineProject } )
    {
        if( tree == nullptr )
            continue;
        tree->setHeaderHidden( true );
        tree->setUniformRowHeights( true );
        connect( tree, &QTreeWidget::itemActivated, this, &MainWindow::onOutlineItemActivated );
        connect( tree, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onOutlineItemActivated );
    }
    setOutlinePlaceholder( Ui.treOutlineDocument, tr( "열린 문서가 없습니다." ), "noDocument" );
    setOutlinePlaceholder( Ui.treOutlineProject, tr( "활성 Sphinx 프로젝트가 없습니다." ), "noProject" );

    connect( controller_, &mrst::WorkspaceController::documentOutlineReady, this,
            [this]( const QString&, const QVector< mrst::OutlineSymbol >& symbols ) {
                QTreeWidget* tree = Ui.treOutlineDocument;
                if( tree == nullptr )
                    return;
                if( symbols.isEmpty() )
                {
                    setOutlinePlaceholder( tree, tr( "문서 심볼이 없습니다." ), "noSymbols" );
                    return;
                }
                tree->clear();
                addOutlineSymbols( nullptr, tree, symbols );
                tree->expandToDepth( 1 );
            } );

    connect( controller_, &mrst::WorkspaceController::projectOutlineReady, this,
            [this]( const QString&, const QVector< mrst::OutlineDocumentEntry >& documents,
                    const int truncated ) {
                QTreeWidget* tree = Ui.treOutlineProject;
                if( tree == nullptr )
                    return;
                if( documents.isEmpty() )
                {
                    setOutlinePlaceholder( tree, tr( "활성 프로젝트에 문서가 없습니다." ), "noProjectDocs" );
                    return;
                }

                tree->clear();
                for( const mrst::OutlineDocumentEntry& document : documents )
                {
                    auto* item = new QTreeWidgetItem( QStringList{ document.label } );
                    item->setData( 0, kOutlinePathRole, document.path );
                    item->setData( 0, kOutlineLineRole, 1 );
                    tree->addTopLevelItem( item );

                    if( document.symbols.isEmpty() )
                        item->addChild( new QTreeWidgetItem( QStringList{ tr( "심볼 없음" ) } ) );
                    else
                        addOutlineSymbols( item, tree, document.symbols );
                }
                if( truncated > 0 )
                {
                    tree->addTopLevelItem( new QTreeWidgetItem(
                        QStringList{ tr( "… %n개 문서 생략", nullptr, truncated ) } ) );
                }
                tree->expandToDepth( 0 );
            } );

    connect( controller_, &mrst::WorkspaceController::outlineCleared, this,
            [this]( const QString& reason ) {
                setOutlinePlaceholder( Ui.treOutlineDocument, reason );
            } );
}

void MainWindow::retranslateOutlinePlaceholders()
{
    // 안내 문구를 보여 주는 중일 때만 손댄다. 실제 심볼이 들어 있으면 그건
    // 문서 내용이라 번역 대상이 아니다.
    const auto retranslate = []( QTreeWidget* tree ) {
        if( tree == nullptr || tree->topLevelItemCount() != 1 )
            return;
        QTreeWidgetItem* item = tree->topLevelItem( 0 );
        const QString    id   = item->data( 0, kOutlinePlaceholderRole ).toString();
        if( id == QLatin1String( "noDocument" ) )
            item->setText( 0, MainWindow::tr( "열린 문서가 없습니다." ) );
        else if( id == QLatin1String( "noProject" ) )
            item->setText( 0, MainWindow::tr( "활성 Sphinx 프로젝트가 없습니다." ) );
        else if( id == QLatin1String( "noSymbols" ) )
            item->setText( 0, MainWindow::tr( "문서 심볼이 없습니다." ) );
        else if( id == QLatin1String( "noProjectDocs" ) )
            item->setText( 0, MainWindow::tr( "활성 프로젝트에 문서가 없습니다." ) );
    };
    retranslate( Ui.treOutlineDocument );
    retranslate( Ui.treOutlineProject );
}

// ═══════════════════════════════════════════════════════════
// 워크스페이스 검색
// ═══════════════════════════════════════════════════════════
void MainWindow::setupWorkspaceSearchTab()
{
    if( Ui.tabStatistics == nullptr )
        return;

    auto* page = new QWidget( Ui.tabStatistics );
    auto* layout = new QVBoxLayout( page );
    layout->setContentsMargins( 4, 4, 4, 4 );
    layout->setSpacing( 4 );

    auto* controls = new QHBoxLayout;
    searchQueryEdit_ = new QLineEdit( page );
    searchQueryEdit_->setPlaceholderText( tr( "찾을 내용" ) );
    searchReplaceEdit_ = new QLineEdit( page );
    searchReplaceEdit_->setPlaceholderText( tr( "바꿀 내용" ) );
    controls->addWidget( searchQueryEdit_, 1 );
    controls->addWidget( searchReplaceEdit_, 1 );

    // 지역 변수지만 retranslateUi() 가 이름으로 찾아 다시 칠한다.
    auto* findButton = new QPushButton( tr( "찾기" ), page );
    findButton->setObjectName( QStringLiteral( "search.find" ) );
    auto* previewButton = new QPushButton( tr( "바꾸기 미리보기" ), page );
    previewButton->setObjectName( QStringLiteral( "search.preview" ) );
    searchApplyButton_ = new QPushButton( tr( "적용" ), page );
    searchApplyButton_->setEnabled( false );
    controls->addWidget( findButton );
    controls->addWidget( previewButton );
    controls->addWidget( searchApplyButton_ );
    layout->addLayout( controls );

    auto* options = new QHBoxLayout;
    searchCaseBox_ = new QCheckBox( tr( "대/소문자 구분" ), page );
    searchWordBox_ = new QCheckBox( tr( "단어 단위" ), page );
    searchRegexBox_ = new QCheckBox( tr( "정규식" ), page );
    searchStatusLabel_ = new QLabel( page );
    options->addWidget( searchCaseBox_ );
    options->addWidget( searchWordBox_ );
    options->addWidget( searchRegexBox_ );
    options->addStretch( 1 );
    options->addWidget( searchStatusLabel_ );
    layout->addLayout( options );

    searchResultTree_ = new QTreeWidget( page );
    searchResultTree_->setHeaderHidden( true );
    searchResultTree_->setUniformRowHeights( true );
    layout->addWidget( searchResultTree_, 1 );

    searchTabPage_ = page;
    Ui.tabStatistics->addTab( page, tr( "검색" ) );

    connect( findButton, &QPushButton::clicked, this, &MainWindow::runWorkspaceSearch );
    connect( searchQueryEdit_, &QLineEdit::returnPressed, this, &MainWindow::runWorkspaceSearch );
    connect( previewButton, &QPushButton::clicked, this, &MainWindow::runWorkspaceReplacePreview );
    connect( searchApplyButton_, &QPushButton::clicked, this, &MainWindow::applyWorkspaceReplace );
    connect( searchResultTree_, &QTreeWidget::itemActivated, this, &MainWindow::onOutlineItemActivated );
    connect( searchResultTree_, &QTreeWidget::itemDoubleClicked, this,
            &MainWindow::onOutlineItemActivated );
}

namespace {

mrst::SearchOptions searchOptionsFrom( const QCheckBox* caseBox, const QCheckBox* wordBox,
                                       const QCheckBox* regexBox )
{
    mrst::SearchOptions options;
    options.caseSensitive = caseBox != nullptr && caseBox->isChecked();
    options.wholeWords = wordBox != nullptr && wordBox->isChecked();
    options.regex = regexBox != nullptr && regexBox->isChecked();
    return options;
}

}  // namespace

void MainWindow::runWorkspaceSearch()
{
    if( searchResultTree_ == nullptr )
        return;

    searchResultTree_->clear();
    pendingReplacePaths_.clear();
    searchApplyButton_->setEnabled( false );

    const QString query = searchQueryEdit_->text();
    if( workspaceRoot_.isEmpty() || query.isEmpty() )
    {
        searchStatusLabel_->setText( tr( "워크스페이스와 찾을 내용을 지정하세요." ) );
        return;
    }

    const QVector< mrst::SearchMatch > matches =
        mrst::findInFiles( workspaceRoot_, query,
                          searchOptionsFrom( searchCaseBox_, searchWordBox_, searchRegexBox_ ) );

    const QDir root( workspaceRoot_ );
    QHash< QString, QTreeWidgetItem* > fileItems;
    for( const mrst::SearchMatch& match : matches )
    {
        QTreeWidgetItem*& fileItem = fileItems[ match.path ];
        if( fileItem == nullptr )
        {
            fileItem = new QTreeWidgetItem( QStringList{ root.relativeFilePath( match.path ) } );
            fileItem->setData( 0, Qt::UserRole, match.path );
            fileItem->setData( 0, Qt::UserRole + 1, 1 );
            searchResultTree_->addTopLevelItem( fileItem );
        }

        auto* hit = new QTreeWidgetItem(
            QStringList{ QStringLiteral( "%1:%2  %3" ).arg( match.line ).arg( match.column ).arg( match.text ) } );
        hit->setData( 0, Qt::UserRole, match.path );
        hit->setData( 0, Qt::UserRole + 1, match.line );
        fileItem->addChild( hit );
    }
    searchResultTree_->expandToDepth( 0 );

    searchStatusLabel_->setText( tr( "파일 %1개에서 %2건" )
                                    .arg( fileItems.size() )
                                    .arg( matches.size() ) );
}

void MainWindow::runWorkspaceReplacePreview()
{
    if( searchResultTree_ == nullptr )
        return;

    searchResultTree_->clear();
    pendingReplacePaths_.clear();
    searchApplyButton_->setEnabled( false );

    const QString query = searchQueryEdit_->text();
    if( workspaceRoot_.isEmpty() || query.isEmpty() )
    {
        searchStatusLabel_->setText( tr( "워크스페이스와 찾을 내용을 지정하세요." ) );
        return;
    }

    const QVector< mrst::ReplacePreview > previews = mrst::previewReplaceInFiles(
        workspaceRoot_, query, searchReplaceEdit_->text(),
        searchOptionsFrom( searchCaseBox_, searchWordBox_, searchRegexBox_ ) );

    const QDir root( workspaceRoot_ );
    int total = 0;
    for( const mrst::ReplacePreview& preview : previews )
    {
        auto* fileItem = new QTreeWidgetItem(
            QStringList{ tr( "%1  (%2건)" ).arg( root.relativeFilePath( preview.path ) )
                             .arg( preview.replacements ) } );
        fileItem->setData( 0, Qt::UserRole, preview.path );
        fileItem->setData( 0, Qt::UserRole + 1, 1 );
        searchResultTree_->addTopLevelItem( fileItem );

        for( const QString& line : preview.diff.split( QLatin1Char( '\n' ) ) )
            fileItem->addChild( new QTreeWidgetItem( QStringList{ line } ) );

        pendingReplacePaths_ << preview.path;
        total += preview.replacements;
    }
    searchResultTree_->expandToDepth( 0 );

    searchApplyButton_->setEnabled( !pendingReplacePaths_.isEmpty() );
    searchStatusLabel_->setText( tr( "미리보기: 파일 %1개, %2건" )
                                    .arg( pendingReplacePaths_.size() )
                                    .arg( total ) );
}

void MainWindow::applyWorkspaceReplace()
{
    if( pendingReplacePaths_.isEmpty() )
        return;

    // 되돌리기가 없는 작업이라 반드시 확인한다.
    if( QMessageBox::question(
            this, tr( "바꾸기 적용" ),
            tr( "파일 %n개를 실제로 바꿉니다. 되돌릴 수 없습니다. 계속할까요?", nullptr,
                static_cast< int >( pendingReplacePaths_.size() ) ) ) != QMessageBox::Yes )
    {
        return;
    }

    const QStringList changed = mrst::applyReplaceInFiles(
        pendingReplacePaths_, searchQueryEdit_->text(), searchReplaceEdit_->text(),
        searchOptionsFrom( searchCaseBox_, searchWordBox_, searchRegexBox_ ) );

    pendingReplacePaths_.clear();
    searchApplyButton_->setEnabled( false );
    searchStatusLabel_->setText( tr( "파일 %n개를 바꿨습니다.", nullptr, static_cast< int >( changed.size() ) ) );
    appendLog( tr( "워크스페이스 바꾸기: 파일 %1개 변경" ).arg( changed.size() ) );

    // 열려 있는 탭은 디스크와 어긋난 상태가 된다. 사용자가 알아야 한다.
    if( !changed.isEmpty() )
    {
        appendLog( tr( "열려 있는 탭은 자동으로 다시 읽지 않습니다. 필요하면 다시 여세요." ) );
    }
}

// ═══════════════════════════════════════════════════════════
// 세션 영속성
// ═══════════════════════════════════════════════════════════
void MainWindow::saveWorkspaceSessionNow()
{
    if( workspaceRoot_.isEmpty() || m_tabWidget == nullptr )
        return;

    mrst::WorkspaceSession session;
    session.workspaceRoot = workspaceRoot_;

    for( int index = 0; index < m_tabWidget->count(); ++index )
    {
        QTextView* view = textViewOf( qobject_cast< QBaseView* >( m_tabWidget->widget( index ) ) );
        if( view == nullptr || view->currentFilePath().isEmpty() )
            continue;   // 이름 없는 버퍼는 hot exit 가 따로 챙긴다

        mrst::OpenDocumentState document;
        document.path = QFileInfo( view->currentFilePath() ).absoluteFilePath();
        document.caretLine = view->caretLine();
        document.caretColumn = view->caretColumn();
        // 복원 쪽(scrollToLine)이 문서 줄로 해석하므로 저장도 문서 줄로 해야 한다.
        // 화면 행을 넣으면 자동 줄넘김이 켜졌을 때 크게 어긋난다.
        document.firstVisibleLine = view->topDocumentLine();

        if( m_tabWidget->currentIndex() == index )
            session.activeIndex = static_cast< int >( session.documents.size() );
        session.documents.push_back( document );
    }

    if( QSplitter* splitter = Ui.splSideWithContent )
        session.sideSplitterSizes = splitter->sizes();
    if( QSplitter* splitter = Ui.splEditWithStatisticsOnContent )
        session.contentSplitterSizes = splitter->sizes();
    if( QSplitter* splitter = Ui.splitter_2 )
        session.previewSplitterSizes = splitter->sizes();

    mrst::saveWorkspaceSession( session );
}

void MainWindow::restoreLastSession()
{
    const mrst::PhaseSpan restoreSpan( "session.restore" );
    const QString lastRoot = AppSettings().value( QStringLiteral( "workspace/lastRoot" ) ).toString();
    if( lastRoot.isEmpty() || !QFileInfo( lastRoot ).isDir() )
        return;

    setWorkspace( lastRoot );

    const mrst::WorkspaceSession session = mrst::loadWorkspaceSession( lastRoot );
    if( session.documents.isEmpty() )
        return;   // 워크스페이스만 되살렸다

    // 탭을 여는 동안에는 활성 문서 반영을 미룬다. addViewTab() 이 탭마다
    // setActiveDocument() 를 부르므로, 미루지 않으면 첫 프리뷰 빌드가 0번 탭
    // 것으로 나가고 사용자가 볼 문서의 빌드는 그것이 끝난 뒤에야 시작한다.
    if( controller_ )
        controller_->beginBatchRestore();

    for( const mrst::OpenDocumentState& document : session.documents )
    {
        if( !QFileInfo::exists( document.path ) )
            continue;   // 그 사이 지워진 파일
        openFile( document.path );
    }

    // 스플리터는 탭을 다 만든 뒤에 적용해야 레이아웃이 다시 계산되며 덮이지 않는다.
    auto restoreSizes = [ this ]( QSplitter* splitter, const QList< int >& sizes ) {
        if( splitter == nullptr || sizes.size() != splitter->count() )
            return false;
        splitter->setSizes( sizes );
        return true;
    };
    restoreSizes( Ui.splSideWithContent, session.sideSplitterSizes );
    restoreSizes( Ui.splEditWithStatisticsOnContent, session.contentSplitterSizes );
    previewSplitFromSession_ = restoreSizes( Ui.splitter_2, session.previewSplitterSizes );

    if( session.activeIndex >= 0 && session.activeIndex < m_tabWidget->count() )
        m_tabWidget->setCurrentIndex( session.activeIndex );

    // setCurrentIndex() 가 인덱스를 바꾸지 않으면(활성 탭이 이미 0번) onTabChanged 가
    // 나지 않는다. 그러면 배치가 기억한 대상이 마지막으로 열린 탭에 머무르므로,
    // 지금 실제 활성 탭을 명시적으로 한 번 더 알려 준다(아직 가드가 켜져 있어
    // 대상만 기록된다). 그 다음 endBatchRestore() 가 한 번만 반영한다.
    if( controller_ )
    {
        controller_->setActiveDocument( textViewOf( currentView() ) );
        controller_->endBatchRestore();
    }

    // 캐럿 복원은 파일 로드가 비동기라 지금 하면 덮인다. 로드가 끝난 뒤에 옮긴다.
    for( int index = 0; index < m_tabWidget->count() && index < session.documents.size(); ++index )
    {
        QTextView* view = textViewOf( qobject_cast< QBaseView* >( m_tabWidget->widget( index ) ) );
        if( view == nullptr )
            continue;

        const mrst::OpenDocumentState state = session.documents.at( index );
        connect( view, &QBaseView::sigFileOpened, this,
                [view, state]( const QString& ) {
                    view->goToPosition( state.caretLine, state.caretColumn );
                    view->scrollToLine( state.firstVisibleLine, 0.0 );
                },
                Qt::SingleShotConnection );
    }
}

void MainWindow::onOutlineItemActivated( QTreeWidgetItem* item, int /*column*/ )
{
    if( item == nullptr )
        return;

    const QString path = item->data( 0, kOutlinePathRole ).toString();
    if( path.isEmpty() )
        return;

    openFile( path );
    if( QTextView* view = textViewOf( currentView() ) )
        view->goToPosition( qMax( 1, item->data( 0, kOutlineLineRole ).toInt() ), 1 );
}

void MainWindow::refreshDiagnosticsTable()
{
    QTableWidget* table = Ui.tblDiagnostics;
    if( table == nullptr || controller_ == nullptr || controller_->diagnostics() == nullptr )
        return;

    const QVector< mrst::DiagnosticEntry > entries = controller_->diagnostics()->all();

    const QSignalBlocker blocker( table );
    table->setRowCount( static_cast< int >( entries.size() ) );

    for( int row = 0; row < entries.size(); ++row )
    {
        const mrst::DiagnosticEntry& entry = entries.at( row );

        auto* severityItem = new QTableWidgetItem( mrst::severityLabel( entry.severity ) );
        severityItem->setData( Qt::UserRole, entry.path );
        severityItem->setData( Qt::UserRole + 1, entry.line );
        if( entry.severity == 1 )
            severityItem->setForeground( QColor( 0xD1, 0x34, 0x38 ) );

        table->setItem( row, 0, severityItem );
        table->setItem( row, 1, new QTableWidgetItem( QFileInfo( entry.path ).fileName() ) );

        auto* lineItem = new QTableWidgetItem( QString::number( entry.line ) );
        lineItem->setTextAlignment( Qt::AlignRight | Qt::AlignVCenter );
        table->setItem( row, 2, lineItem );

        table->setItem( row, 3, new QTableWidgetItem( entry.message ) );
        table->setItem( row, 4, new QTableWidgetItem( entry.source ) );

        // 전체 경로는 툴팁으로 (파일명만으로는 멀티루트에서 구분이 안 된다).
        table->item( row, 1 )->setToolTip( QDir::toNativeSeparators( entry.path ) );
    }

    // 출처별 내역까지 남긴다. 같은 위치면 esbonio 가 sphinx-build 를 대체하므로,
    // 내역을 봐야 어느 쪽이 실제로 표시되고 있는지 알 수 있다.
    QMap< QString, int > bySource;
    for( const mrst::DiagnosticEntry& entry : entries )
        ++bySource[ entry.source ];

    QStringList breakdown;
    for( auto it = bySource.constBegin(); it != bySource.constEnd(); ++it )
        breakdown << QStringLiteral( "%1 %2" ).arg( it.key() ).arg( it.value() );

    appendLog( breakdown.isEmpty()
                  ? tr( "진단 0건" )
                  : tr( "진단 %1건 (%2)" ).arg( entries.size() ).arg( breakdown.join( QStringLiteral( ", " ) ) ) );
}

void MainWindow::setupMissingDependencyBar()
{
    // 프리뷰 위에 얇게 얹는다. 모달이 아니라서 무시하고 계속 편집할 수 있다.
    QWidget* host = Ui.frmWebPreview;
    if( host == nullptr || host->layout() == nullptr )
        return;

    missingDepBar_ = new QWidget( host );
    missingDepBar_->setVisible( false );
    missingDepBar_->setAutoFillBackground( true );
    missingDepBar_->setStyleSheet(
        QStringLiteral( "background-color: palette(alternate-base); border-bottom: 1px solid palette(mid);" ) );

    auto* layout = new QHBoxLayout( missingDepBar_ );
    layout->setContentsMargins( 8, 4, 8, 4 );

    missingDepLabel_ = new QLabel( missingDepBar_ );
    missingDepLabel_->setWordWrap( true );
    layout->addWidget( missingDepLabel_, 1 );

    auto* installButton = new QPushButton( tr( "설치" ), missingDepBar_ );
    installButton->setObjectName( QStringLiteral( "missingDep.install" ) );
    auto* ignoreButton = new QPushButton( tr( "무시" ), missingDepBar_ );
    ignoreButton->setObjectName( QStringLiteral( "missingDep.ignore" ) );
    layout->addWidget( installButton );
    layout->addWidget( ignoreButton );

    connect( installButton, &QPushButton::clicked, this, [this] {
        if( pythonEnv_ == nullptr || missingDepPending_.isEmpty() )
            return;
        // 사용자 venv 를 함부로 건드리지 않는다. 내장 환경에만 설치한다.
        pythonEnv_->installPackagesAsync( missingDepPending_ );
        missingDepBar_->setVisible( false );
    } );
    connect( ignoreButton, &QPushButton::clicked, this, [this] {
        missingDepDismissed_ += missingDepPending_;
        missingDepPending_.clear();
        missingDepBar_->setVisible( false );
    } );

    host->layout()->addWidget( missingDepBar_ );
}

void MainWindow::showMissingDependencies( const QStringList& distributions )
{
    if( missingDepBar_ == nullptr )
        return;

    QStringList fresh;
    for( const QString& name : distributions )
    {
        if( !missingDepDismissed_.contains( name ) )
            fresh << name;
    }
    if( fresh.isEmpty() )
        return;

    missingDepPending_ = fresh;
    missingDepLabel_->setText( tr( "Sphinx 확장/테마를 찾을 수 없습니다: %1" )
                                  .arg( fresh.join( QStringLiteral( ", " ) ) ) );
    missingDepBar_->setVisible( true );
}

void MainWindow::setupPythonEnvironment()
{
    pythonEnv_ = new mrst::PythonEnvManager( this );
    if( controller_ != nullptr )
        controller_->setPythonEnvironment( pythonEnv_ );

    // 프리뷰 칩이 환경 칩보다 왼쪽에 오도록 먼저 붙인다.
    previewStatusLabel_ = new QLabel( this );
    previewStatusLabel_->setContentsMargins( 8, 0, 8, 0 );
    previewStatusLabel_->setVisible( false );
    statusBar()->addPermanentWidget( previewStatusLabel_ );

    envStatusLabel_ = new QLabel( this );
    envStatusLabel_->setContentsMargins( 8, 0, 8, 0 );
    statusBar()->addPermanentWidget( envStatusLabel_ );

    if( controller_ != nullptr )
    {
        connect( controller_, &mrst::WorkspaceController::previewStatusChanged, this,
                [this]( const QString& text, const bool busy ) {
                    if( previewStatusLabel_ == nullptr )
                        return;
                    previewStatusLabel_->setText( text );
                    previewStatusLabel_->setVisible( busy );
                } );
    }

    connect( pythonEnv_, &mrst::PythonEnvManager::bootstrapLog, this, &MainWindow::appendLog );
    connect( pythonEnv_, &mrst::PythonEnvManager::stateChanged, this,
            [this]( mrst::EnvState ) { updateEnvStatusChip(); } );
    connect( pythonEnv_, &mrst::PythonEnvManager::packageInstallFinished, this,
            [this]( const bool ok, const QStringList& distributions ) {
                appendLog( ok ? tr( "패키지 설치 완료: %1" ).arg( distributions.join( QStringLiteral( ", " ) ) )
                              : tr( "패키지 설치 실패: %1" ).arg( distributions.join( QStringLiteral( ", " ) ) ) );
                if( ok && controller_ != nullptr )
                    controller_->requestPreviewBuild( true );
            } );
    connect( pythonEnv_, &mrst::PythonEnvManager::readyChanged, this, [this]( const bool ready ) {
        // 런타임이 준비되기 전에 열린 문서는 프리뷰/LSP 를 건너뛰었으므로 지금 시도한다.
        if( ready && controller_ != nullptr )
            controller_->setActiveDocument( textViewOf( currentView() ) );
    } );
    connect( pythonEnv_, &mrst::PythonEnvManager::progressChanged, this,
            [this]( const int percent, const QString& phase ) {
                if( envStatusLabel_ == nullptr )
                    return;
                envStatusLabel_->setText( percent < 0
                                             ? tr( "환경: %1" ).arg( phase )
                                             : tr( "환경: %1 (%2%)" ).arg( phase ).arg( percent ) );
            } );

    updateEnvStatusChip();

    // startup 을 막지 않는다. 창이 뜬 뒤에 백그라운드로 시작한다.
    if( pythonEnv_->autoBootstrap() && !pythonEnv_->isReady() )
        QTimer::singleShot( 0, this, [this] { pythonEnv_->ensureEnvironmentAsync(); } );
}

void MainWindow::setupUpdateBar()
{
    // missingDepBar_ 는 Ui.frmWebPreview 안에 있어 프리뷰 위에만 뜬다. 업데이트는
    // 앱 전역의 사건이므로 도구모음 슬롯 바로 아래, 창 폭 전체를 쓰는 자리에 둔다.
    auto* containerLayout = qobject_cast< QVBoxLayout* >(
        m_centralContainer != nullptr ? m_centralContainer->layout() : nullptr );
    if( containerLayout == nullptr )
        return;

    updateBar_ = new QWidget( m_centralContainer );
    updateBar_->setVisible( false );
    updateBar_->setAutoFillBackground( true );
    updateBar_->setStyleSheet(
        QStringLiteral( "background-color: palette(alternate-base); border-bottom: 1px solid palette(mid);" ) );

    auto* layout = new QHBoxLayout( updateBar_ );
    layout->setContentsMargins( 8, 4, 8, 4 );

    updateLabel_ = new QLabel( updateBar_ );
    updateLabel_->setWordWrap( true );
    layout->addWidget( updateLabel_, 1 );

    updateNotesButton_  = new QPushButton( tr( "릴리스 노트" ), updateBar_ );
    updateActionButton_ = new QPushButton( updateBar_ );
    updateSkipButton_   = new QPushButton( tr( "이 버전 건너뛰기" ), updateBar_ );
    updateLaterButton_  = new QPushButton( tr( "나중에" ), updateBar_ );
    layout->addWidget( updateNotesButton_ );
    layout->addWidget( updateActionButton_ );
    layout->addWidget( updateSkipButton_ );
    layout->addWidget( updateLaterButton_ );

    connect( updateActionButton_, &QPushButton::clicked, this, [this] {
        if( updateService_ == nullptr )
            return;
        // 진행 상황은 상태 표시줄 칩이 보여 준다. 바는 접는다.
        updateBar_->setVisible( false );
        if( updateService_->state() == mrst::UpdateService::State::ReadyToInstall )
            confirmInstallNow();
        else
            updateService_->downloadAsync();
    } );
    connect( updateLaterButton_, &QPushButton::clicked, this, [this] {
        // 이번 세션만 조용히 한다. 다음 실행에서는 다시 알린다.
        if( updateService_ != nullptr )
            updateDismissedVersion_ = updateService_->available().version;
        updateBar_->setVisible( false );
    } );
    connect( updateSkipButton_, &QPushButton::clicked, this, [this] {
        if( updateService_ != nullptr )
            updateService_->skipAvailableVersion();
        updateBar_->setVisible( false );
    } );
    connect( updateNotesButton_, &QPushButton::clicked, this, [this] {
        if( updateService_ == nullptr )
            return;
        const QUrl notes = updateService_->available().notesUrl;
        if( notes.isValid() )
            QDesktopServices::openUrl( notes );
    } );

    // index 0 은 뷰어 도구모음 슬롯이다. 그 바로 아래에 끼운다.
    containerLayout->insertWidget( 1, updateBar_ );
}

void MainWindow::setupUpdateService()
{
    updateService_ = new mrst::UpdateService( this );

    updateStatusLabel_ = new QLabel( this );
    updateStatusLabel_->setContentsMargins( 8, 0, 8, 0 );
    updateStatusLabel_->setVisible( false );
    statusBar()->addPermanentWidget( updateStatusLabel_ );

    connect( updateService_, &mrst::UpdateService::logMessage, this, &MainWindow::appendLog );
    connect( updateService_, &mrst::UpdateService::updateFound, this, &MainWindow::showUpdateAvailable );
    connect( updateService_, &mrst::UpdateService::readyToInstall, this,
            [this]( const mrst::UpdateInfo& info ) { showUpdateReady( info.version ); } );
    connect( updateService_, &mrst::UpdateService::progressChanged, this,
            [this]( const int percent, const QString& phase ) {
                if( updateStatusLabel_ == nullptr )
                    return;
                updateStatusLabel_->setVisible( true );
                updateStatusLabel_->setText( percent < 0
                                                ? tr( "업데이트: %1" ).arg( phase )
                                                : tr( "업데이트: %1 (%2%)" ).arg( phase ).arg( percent ) );
            } );
    connect( updateService_, &mrst::UpdateService::stateChanged, this,
            [this]( mrst::UpdateService::State ) {
                if( updateStatusLabel_ != nullptr && !updateService_->isBusy() )
                    updateStatusLabel_->setVisible( false );
            } );
    connect( updateService_, &mrst::UpdateService::upToDate, this, [this]( const bool userInitiated ) {
        if( userInitiated )
            statusBar()->showMessage( tr( "최신 버전을 사용하고 있습니다." ), 4000 );
    } );
    connect( updateService_, &mrst::UpdateService::failed, this,
            [this]( const QString& message, const bool silent ) {
                if( !silent )
                    QMessageBox::warning( this, tr( "업데이트" ), message );
            } );
    connect( updateService_, &mrst::UpdateService::installOutcomeReported, this,
            [this]( const bool succeeded, const QString& version, const QString& message ) {
                if( succeeded )
                    statusBar()->showMessage( tr( "%1 로 업데이트했습니다." ).arg( version ), 8000 );
                else
                    QMessageBox::warning( this, tr( "업데이트" ),
                                         tr( "업데이트를 적용하지 못했습니다.\n%1" ).arg( message ) );
            } );

    // startup 을 막지 않는다 (setupPythonEnvironment 와 같은 관용구).
    // 지난 설치 결과 확인과 뒷정리는 먼저, 네트워크 점검은 조금 뒤에 한다 —
    // 파이썬 부트스트랩이 수백 MB 를 받는 중이면 대역폭을 나눠 쓰게 된다.
    QTimer::singleShot( 0, this, [this] { updateService_->reconcileAfterRestart(); } );
    QTimer::singleShot( kUpdateFirstCheckDelayMs, this, [this] {
        if( updateService_->isDueForCheck() )
            updateService_->checkAsync( /*userInitiated=*/false );
    } );

    // 편집기는 며칠씩 켜 두는 앱이다. 기동 시 한 번만 보면 "7일마다" 라는 설정이
    // 사실상 "실행할 때마다" 가 된다.
    auto* heartbeat = new QTimer( this );
    heartbeat->setInterval( kUpdateHeartbeatMs );
    connect( heartbeat, &QTimer::timeout, this, [this] {
        if( !updateService_->isBusy() && updateService_->isDueForCheck() )
            updateService_->checkAsync( /*userInitiated=*/false );
    } );
    heartbeat->start();
}

void MainWindow::showUpdateAvailable( const mrst::UpdateInfo& info )
{
    if( updateBar_ == nullptr )
        return;
    // 이번 세션에 "나중에" 를 누른 버전은 다시 띄우지 않는다.
    if( !updateDismissedVersion_.isEmpty() && updateDismissedVersion_ == info.version )
        return;

    updateLabel_->setText( tr( "새 버전 %1 이 있습니다 (약 %2MB)." )
                              .arg( info.version )
                              .arg( info.asset.size / 1048576.0, 0, 'f', 0 ) );
    updateActionButton_->setText( tr( "내려받기" ) );
    updateNotesButton_->setVisible( info.notesUrl.isValid() );
    updateSkipButton_->setVisible( true );
    updateBar_->setVisible( true );
}

void MainWindow::showUpdateReady( const QString& version )
{
    if( updateBar_ == nullptr )
        return;

    updateLabel_->setText( tr( "%1 설치 준비가 끝났습니다. 앱을 다시 시작하면 적용됩니다." )
                              .arg( version ) );
    updateActionButton_->setText( tr( "지금 재시작" ) );
    updateNotesButton_->setVisible( false );
    // 이미 내려받아 둔 것을 건너뛰게 하면 그 파일이 갈 곳이 없다.
    updateSkipButton_->setVisible( false );
    updateBar_->setVisible( true );
}

void MainWindow::confirmInstallNow()
{
    if( updateService_ == nullptr )
        return;

    const auto answer = QMessageBox::question(
        this, tr( "업데이트 설치" ),
        tr( "새 버전 %1 을 설치할 준비가 되었습니다.\n\n"
            "앱을 닫고 파일을 교체한 뒤 다시 실행합니다.\n"
            "저장하지 않은 문서가 있으면 먼저 물어봅니다. 계속할까요?" )
           .arg( updateService_->available().version ),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No );
    if( answer != QMessageBox::Yes )
        return;

    // 업데이터를 여기서 띄우면 안 된다. closeEvent 의 저장 확인에서 사용자가
    // 취소할 수 있고, 그러면 업데이터는 죽지 않을 pid 를 90초 동안 기다린다.
    pendingInstall_ = true;
    close();
}

void MainWindow::updateEnvStatusChip()
{
    if( envStatusLabel_ == nullptr || pythonEnv_ == nullptr )
        return;

    envStatusLabel_->setText( tr( "환경: %1" ).arg( pythonEnv_->stateText() ) );
    envStatusLabel_->setToolTip( pythonEnv_->isReady()
                                    ? QDir::toNativeSeparators( pythonEnv_->pythonExe() )
                                    : pythonEnv_->lastError() );
}

void MainWindow::openStartupPaths( const QStringList& paths )
{
    if( paths.isEmpty() )
        return;

    const QFileInfo first( paths.first() );
    if( !first.exists() )
        return;

    // 파일이 첫 인자면 상위 폴더를 워크스페이스로 삼아야 프로젝트 스캔이 동작한다.
    setWorkspace( first.isDir() ? first.absoluteFilePath() : first.absolutePath() );

    // 세션 복원과 같은 이유로 활성 문서 반영을 마지막 한 번으로 접는다.
    if( controller_ )
        controller_->beginBatchRestore();

    for( const QString& path : paths )
    {
        const QFileInfo info( path );
        if( info.exists() && info.isFile() )
            openFile( info.absoluteFilePath() );
    }

    if( controller_ )
    {
        controller_->setActiveDocument( textViewOf( currentView() ) );
        controller_->endBatchRestore();
    }
}
