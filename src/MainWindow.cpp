#include "stdafx.h"
#include "MainWindow.hpp"

#include "core/solAppSettings.hpp"
#include "core/solBaseView.hpp"
#include "core/solThemeManager.hpp"
#include "core/solShadowBackupStore.hpp"
#include "editor/QBaseEditor.hpp"
#include "uis/dlgSettings.hpp"

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
#include <QTimer>
#include <QUrl>

#include <QVector>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
    enum class NewFileTarget
    {
        Text,
        Image
    };

    enum class ClipboardImagePastePolicy
    {
        Ask = 0,
        AlwaysOpen = 1,
        AlwaysIgnore = 2
    };

    constexpr auto kClipboardImagePastePolicyKey = "clipboard/imagePastePolicy";

    QString clipboardImagePastePolicyKey()
    {
        return QString::fromLatin1( kClipboardImagePastePolicyKey );
    }

    ClipboardImagePastePolicy loadClipboardImagePastePolicy()
    {
        AppSettings settings;
        return static_cast< ClipboardImagePastePolicy >(
            settings.value( clipboardImagePastePolicyKey(), static_cast< int >( ClipboardImagePastePolicy::Ask ) ).toInt() );
    }

    void saveClipboardImagePastePolicy( ClipboardImagePastePolicy policy )
    {
        AppSettings settings;

        settings.setValue( clipboardImagePastePolicyKey(), static_cast< int >( policy ) );
    }

    QStringList imageFileExtensions()
    {
        return { "jpg", "jpeg", "png", "bmp", "gif", "tiff", "tif", "ico", "webp", "svg" };
    }

    QStringList markdownFileExtensions()
    {
        return { "md", "markdown", "mdown" };
    }

    bool fileWouldOpenAsText( const QString& filePath )
    {
        const QString ext = QFileInfo( filePath ).suffix().toLower();
        return ext != QStringLiteral( "pdf" )
            && !imageFileExtensions().contains( ext )
            && !markdownFileExtensions().contains( ext );
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

    QString clipboardPasteContextText( QBaseView* view )
    {
        return {};
        //if( qobject_cast< QTextView* >( view ) )
        //    return QObject::tr( "텍스트" );
        //if( qobject_cast< QMarkdownView* >( view ) )
        //    return QObject::tr( "Markdown" );
        //if( qobject_cast< QImageView* >( view ) )
        //    return QObject::tr( "이미지" );
        //return QObject::tr( "현재" );
    }

    QString clipboardImagePastePolicyText( ClipboardImagePastePolicy policy )
    {
        switch( policy )
        {
            case ClipboardImagePastePolicy::AlwaysOpen:
                return QObject::tr( "항상 열기" );
            case ClipboardImagePastePolicy::AlwaysIgnore:
                return QObject::tr( "항상 무시" );
            case ClipboardImagePastePolicy::Ask:
            default:
                return QObject::tr( "매번 확인" );
        }
    }

    bool promptNewFileTarget( QWidget* parent, NewFileTarget* target )
    {
        if( !target )
            return false;

        QMessageBox box( QMessageBox::Question,
                        QObject::tr( "새 파일" ),
                        QObject::tr( "새 파일에서 시작할 작업 유형을 선택하세요." ),
                        QMessageBox::NoButton,
                        parent );
        box.setInformativeText(
            QObject::tr( "텍스트 뷰어는 빈 문서로, 이미지 뷰어는 빈 캔버스(800 × 600)로 시작합니다." ) );

        auto* textButton = box.addButton( QObject::tr( "텍스트 뷰어" ), QMessageBox::AcceptRole );
        auto* imageButton = box.addButton( QObject::tr( "이미지 뷰어" ), QMessageBox::ActionRole );
        auto* cancelButton = box.addButton( QMessageBox::Cancel );

        box.setDefaultButton( textButton );
        box.setEscapeButton( cancelButton );
        box.exec();

        if( box.clickedButton() == textButton )
        {
            *target = NewFileTarget::Text;
            return true;
        }

        if( box.clickedButton() == imageButton )
        {
            *target = NewFileTarget::Image;
            return true;
        }

        return false;
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
    Ui.setupUi( this );
    setWindowTitle( tr( "MultiRoot reST Editor" ) );
    resize( 1024, 768 );
    setAcceptDrops( true );
    if( auto* app = QCoreApplication::instance() )
        app->installEventFilter( this );

    centralWidget()->setAcceptDrops( true );
    centralWidget()->setAttribute( Qt::WA_OpaquePaintEvent );

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
    m_tabWidget->setAttribute( Qt::WA_OpaquePaintEvent );

    Ui.webEngineView->setHtml( QStringLiteral( "<h1>MultiRoot reST C++ Port</h1><p>C++/Qt 전환 셸이 시작되었습니다.</p>" ) );

    Ui.splFolderWithOutlineOnSide->setMinimumWidth( 200 );
    Ui.frmBottom->setMinimumHeight( 150 );
    Ui.splEditWithStatisticsOnContent->setStretchFactor( 0, 1 );
    Ui.splEditWithStatisticsOnContent->setStretchFactor( 1, 0 );
    Ui.splSideWithContent->setStretchFactor( 0, 0 );
    Ui.splSideWithContent->setStretchFactor( 1, 1 );

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

    if( settings.value( "textView/hotExitEnabled", true ).toBool() )
    {
        const QList<TextShadowBackupStore::Snapshot> hotExitSnapshots = TextShadowBackupStore::restorableSnapshots( false );
        for( const TextShadowBackupStore::Snapshot& snapshot : hotExitSnapshots )
        {
            if( snapshot.isUntitled )
            {
                //auto* view = new QTextView( this );
                //applyPersistedViewSettings( view );
                //applyThemeToView( view );
                //if( !view->openHotExitBackup( snapshot.untitledId ) )
                //{
                //    delete view;
                //    continue;
                //}
                //addViewTab( view );
                //continue;
            }

            openFile( snapshot.originalFilePath );
        }
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
void MainWindow::createMenus()
{
    auto* fileMenu = menuBar()->addMenu( tr( "파일(&F)" ) );

    auto* newAction = fileMenu->addAction( tr( "새 파일(&N)" ), this, &MainWindow::onFileNew );
    newAction->setObjectName( QStringLiteral( "file.new" ) );
    newAction->setProperty( "mv.shortcutId", QStringLiteral( "file.new" ) );
    newAction->setShortcut( QKeySequence::New );
    newAction->setShortcutContext( Qt::ApplicationShortcut );

    auto* openAction = fileMenu->addAction( tr( "열기(&O)..." ), QKeySequence::Open, this, &MainWindow::onFileOpen );
    openAction->setObjectName( QStringLiteral( "file.open" ) );

    m_saveAction = fileMenu->addAction( tr( "저장(&S)" ), this, &MainWindow::onFileSave );
    m_saveAction->setObjectName( QStringLiteral( "file.save" ) );
    m_saveAction->setProperty( "mv.shortcutId", QStringLiteral( "file.save" ) );
    m_saveAction->setShortcut( QKeySequence::Save );
    m_saveAction->setShortcutContext( Qt::ApplicationShortcut );

    m_saveAsAction = fileMenu->addAction( tr( "다른 이름으로 저장(&A)..." ), this, &MainWindow::onFileSaveAs );
    m_saveAsAction->setObjectName( QStringLiteral( "file.saveAs" ) );
    m_saveAsAction->setProperty( "mv.shortcutId", QStringLiteral( "file.saveAs" ) );
    m_saveAsAction->setShortcut( QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_S ) );
    m_saveAsAction->setShortcutContext( Qt::ApplicationShortcut );

    m_recentMenu = fileMenu->addMenu( tr( "최근 파일(&R)" ) );

    fileMenu->addSeparator();
    auto* closeTabAction = fileMenu->addAction( tr( "현재 탭 닫기(&C)" ), this, [this] {
        if( m_tabWidget )
            onCloseTab( m_tabWidget->currentIndex() );
    } );
    closeTabAction->setObjectName( QStringLiteral( "tab.close" ) );
    closeTabAction->setProperty( "mv.shortcutId", QStringLiteral( "tab.close" ) );
    closeTabAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_W ) );
    closeTabAction->setShortcutContext( Qt::ApplicationShortcut );

    fileMenu->addSeparator();
    fileMenu->addAction( tr( "종료(&X)" ), QKeySequence::Quit, QApplication::instance(), &QApplication::quit );

    auto* editMenu = menuBar()->addMenu( tr( "편집(&E)" ) );
    m_copyAction = editMenu->addAction( tr( "복사(&C)" ), this, &MainWindow::onCopy );
    m_copyAction->setShortcut( QKeySequence::Copy );
    m_copyAction->setShortcutContext( Qt::ApplicationShortcut );
    m_copyAction->setEnabled( false );
    m_pasteAction = editMenu->addAction( tr( "붙여넣기(&P)" ), this, &MainWindow::onPaste );
    m_pasteAction->setShortcut( QKeySequence::Paste );
    m_pasteAction->setShortcutContext( Qt::ApplicationShortcut );
    m_pasteAction->setEnabled( false );

    auto* viewMenu = menuBar()->addMenu( tr( "보기(&V)" ) );
    viewMenu->addAction( tr( "테마 전환" ), this, &MainWindow::onThemeToggle );

    auto* settingsMenu = menuBar()->addMenu( tr( "설정(&S)" ) );
    auto* settingsAction = settingsMenu->addAction( tr( "설정(&I)..." ), this, &MainWindow::onSettings );
    settingsAction->setObjectName( QStringLiteral( "app.settings" ) );
    settingsAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_I ) );
    settingsAction->setShortcutContext( Qt::ApplicationShortcut );
    settingsMenu->addSeparator();
    settingsMenu->addAction( tr( "클립보드 이미지 붙여넣기 확인 다시 켜기" ),
                            this, &MainWindow::onResetClipboardImagePastePrompt );
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

    m_viewerToolBar = view->createToolBar();
    if( m_viewerToolBar )
    {
        m_viewerToolBar->setParent( m_viewerToolBarHost );
        m_viewerToolBar->setObjectName( "viewerToolBar" );

        m_viewerToolBar->addSeparator();
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

    //const auto theme = ThemeManager::instance().currentTheme() == ThemeManager::Dark
    //    ? QBaseView::Dark
    //    : QBaseView::Light;
    //view->setTheme( theme );

    //if( auto* markdownView = qobject_cast< QMarkdownView* >( view ) )
    //    markdownView->refreshPreview();
}

void MainWindow::applyCurrentTheme()
{
    //auto& themeManager = ThemeManager::instance();
    //DwmTitleBar::applyTheme( this,
    //                        themeManager.currentTheme() == ThemeManager::Dark,
    //                        themeManager.toolBarColor() );

    //for( int i = 0; i < m_tabWidget->count(); ++i )
    //{
    //    if( auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( i ) ) )
    //    {
    //        applyThemeToView( view );
    //        updateTabDecoration( view );
    //    }
    //}

    //updateViewerToolBar();
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

    //if( imageFileExtensions().contains( ext ) )
    //    return new QImageView( this );

    //if( markdownFileExtensions().contains( ext ) )
    //    return new QMarkdownView( this );

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
    //else if( auto* textView = qobject_cast< QTextView* >( view ) )
    //{
    //    // 텍스트 뷰어는 자체적으로 loadPersistedEditorPreferences()를 호출하므로
    //    // 여기서는 글꼴과 행간 등 추가 설정만 적용
    //    const QString fontFamily = s.value( "textView/fontFamily", "Consolas" ).toString();
    //    const int fontSize = qBound( 6, s.value( "textView/fontSize", 10 ).toInt(), 72 );
    //    textView->setEditorFont( QFont( fontFamily, fontSize ) );
    //    textView->setLineSpacingScale( qBound( 1.0, s.value( "textView/lineSpacing", 1.1 ).toDouble(), 3.0 ) );
    //}
}

void MainWindow::applySettingsToAllViews()
{
    if( !m_tabWidget )
        return;

    AppSettings s;

    //for( int i = 0; i < m_tabWidget->count(); ++i )
    //{
    //    auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( i ) );
    //    if( !view )
    //        continue;

    //    if( auto* textView = qobject_cast< QTextView* >( view ) )
    //    {
    //        const QString fontFamily = s.value( "textView/fontFamily", "Consolas" ).toString();
    //        const int fontSize = qBound( 6, s.value( "textView/fontSize", 10 ).toInt(), 72 );
    //        textView->setEditorFont( QFont( fontFamily, fontSize ) );
    //        textView->setLineSpacingScale( qBound( 1.0, s.value( "textView/lineSpacing", 1.1 ).toDouble(), 3.0 ) );
    //        textView->setTabWidth( qBound( 1, s.value( "textView/tabWidth", 4 ).toInt(), 16 ) );
    //        textView->setUseTabs( s.value( "textView/useTabs", true ).toBool() );
    //        textView->setIndentationGuidesVisible( s.value( "textView/showIndentationGuides", true ).toBool() );
    //        textView->setIndentGuideStyle( static_cast< ScintillaEditorSettings::IndentGuideStyle >(
    //            qBound( 1, s.value( "textView/indentGuideStyle", 1 ).toInt(), 3 ) ) );
    //        textView->setWhitespaceVisible( s.value( "textView/showWhitespace", false ).toBool() );
    //        textView->setCodeFoldingEnabled( s.value( "textView/showCodeFolding", true ).toBool() );
    //        textView->setBraceHighlightEnabled( s.value( "textView/braceHighlight", true ).toBool() );
    //        const int fontRendering = s.value( "textView/fontRendering", 2 ).toInt();
    //        textView->setFontRenderingMode( static_cast< ScintillaEditorSettings::FontRenderingMode >(
    //            qBound( 0, fontRendering, 3 ) ) );
    //        const int changeHistory = s.value( "textView/changeHistoryMode", 3 ).toInt();
    //        textView->setChangeHistoryMode( static_cast< ScintillaEditorSettings::ChangeHistoryMode >(
    //            qBound( 0, changeHistory, 3 ) ) );
    //        textView->setHotExitEnabled( s.value( "textView/hotExitEnabled", true ).toBool() );
    //    }
    //    else if( auto* imageView = qobject_cast< QImageView* >( view ) )
    //    {
    //        const QColor drawColor( s.value( "image/drawColor", ThemeManager::instance().color( QStringLiteral( "image.draw" ) ).name() ).toString() );
    //        if( drawColor.isValid() )
    //            imageView->setPenColor( drawColor );
    //        imageView->setPenWidth( qBound( 1, s.value( "image/drawWidth", 2 ).toInt(), 20 ) );
    //    }
    //    // PDF 뷰 모드는 이미 열린 문서에는 적용하지 않음 (사용자가 수동 변경 가능)
    //}
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

    //connect( view, &QBaseView::titleChanged, this, [this, view]( const QString& title ) {
    //    const int i = m_tabWidget ? m_tabWidget->indexOf( view ) : -1;
    //    if( i >= 0 )
    //        updateTabDecoration( view );
    //    if( currentView() == view )
    //        updateTitle();
    //} );

    //connect( view, &QBaseView::modifiedChanged, this, [this, view]( bool ) {
    //    updateTabDecoration( view );
    //    if( currentView() == view )
    //        updateTitle();
    //} );

    connectViewStatusSignals( view );

    updateCopyActionState();
    updatePasteActionState();
    return idx;
}

void MainWindow::disconnectViewSignals( QBaseView* view )
{
    if( !view )
        return;

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

    //connect( view, &QBaseView::loadingStateChanged, this,
    //        [this, view]( bool active, const QString& message, int value, int maximum ) {
    //            setViewLoadingState( view, active, message, value, maximum );
    //        } );
    //connect( view, &QObject::destroyed, this, [this, view] {
    //    m_activeViewLoads.remove( view );
    //    refreshLoadingIndicator();
    //} );

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

    //if( auto* textView = qobject_cast< QTextView* >( view ) )
    //{
    //    connect( textView, &QTextView::statusChanged, this, [this, textView] {
    //        if( currentView() == textView )
    //            updateStatusBar();
    //    } );
    //    connect( textView, &QTextView::encodingChanged, this, [this, textView] {
    //        if( currentView() == textView )
    //            updateStatusBar();
    //    } );
    //    connect( textView, &QTextView::languageChanged, this, [this, textView] {
    //        if( currentView() == textView )
    //            updateStatusBar();
    //    } );
    //}
}

bool MainWindow::canPasteClipboardImage() const
{
    if( !canPasteClipboardImageInCurrentContext() )
        return false;

    const QClipboard* clipboard = QApplication::clipboard();
    if( !clipboard )
        return false;

    const QMimeData* mimeData = clipboard->mimeData();
    if( !mimeData || !mimeData->hasImage() )
        return false;

    const QVariant imageData = mimeData->imageData();
    if( imageData.canConvert<QImage>() )
        return !qvariant_cast< QImage >( imageData ).isNull();
    if( imageData.canConvert<QPixmap>() )
        return !qvariant_cast< QPixmap >( imageData ).isNull();

    return !clipboard->image().isNull();
}

bool MainWindow::canPasteClipboardImageInCurrentContext() const
{
    if( !m_tabWidget )
        return false;

    if( m_tabWidget->count() == 0 )
        return true;

    QBaseView* view = currentView();
    //return qobject_cast< QImageView* >( view )
    //    || qobject_cast< QPDFView* >( view )
    //    || qobject_cast< QTextView* >( view )
    //    || qobject_cast< QMarkdownView* >( view );
    return false;
}

bool MainWindow::shouldConfirmClipboardImageOpen() const
{
    QBaseView* view = currentView();
    //return qobject_cast< QPDFView* >( view )
    //    || qobject_cast< QTextView* >( view )
    //    || qobject_cast< QMarkdownView* >( view );
    return false;
}

bool MainWindow::confirmOpenClipboardImage() const
{
    const ClipboardImagePastePolicy policy = loadClipboardImagePastePolicy();
    if( policy == ClipboardImagePastePolicy::AlwaysOpen )
        return true;
    if( policy == ClipboardImagePastePolicy::AlwaysIgnore )
        return false;

    QBaseView* view = currentView();
    const QString contextText = clipboardPasteContextText( view );
    const QString currentTabTitle = view ? view->title() : tr( "현재 탭" );

    QMessageBox box( QMessageBox::Question,
                    tr( "클립보드 이미지 붙여넣기" ),
                    tr( "클립보드에 이미지 데이터가 있습니다." ),
                    QMessageBox::NoButton,
                    const_cast< MainWindow* >( this ) );
    box.setInformativeText(
        tr( "현재는 %1 탭 \"%2\"이(가) 활성화되어 있습니다.\n이미지를 새 이미지 뷰어 탭으로 여시겠습니까?" )
            .arg( contextText, currentTabTitle ) );
    box.setDetailedText( tr( "선택하면 현재 탭 내용은 변경되지 않고, 새 이미지 뷰어 탭이 추가됩니다." ) );

    auto* openOnceButton = box.addButton( tr( "이번만 열기" ), QMessageBox::AcceptRole );
    auto* alwaysOpenButton = box.addButton( tr( "항상 열기" ), QMessageBox::YesRole );
    auto* alwaysIgnoreButton = box.addButton( tr( "항상 무시" ), QMessageBox::DestructiveRole );
    box.setDefaultButton( openOnceButton );
    box.setEscapeButton( alwaysIgnoreButton );
    box.exec();

    if( box.clickedButton() == alwaysOpenButton )
    {
        saveClipboardImagePastePolicy( ClipboardImagePastePolicy::AlwaysOpen );
        return true;
    }

    if( box.clickedButton() == alwaysIgnoreButton )
    {
        saveClipboardImagePastePolicy( ClipboardImagePastePolicy::AlwaysIgnore );
        return false;
    }

    return box.clickedButton() == openOnceButton;
}

bool MainWindow::openClipboardImage()
{
    if( !canPasteClipboardImage() )
        return false;

    const QClipboard* clipboard = QApplication::clipboard();
    if( !clipboard )
        return false;

    QImage image = clipboard->image();
    if( image.isNull() )
    {
        const QMimeData* mimeData = clipboard->mimeData();
        if( mimeData && mimeData->hasImage() )
        {
            const QVariant imageData = mimeData->imageData();
            if( imageData.canConvert<QImage>() )
                image = qvariant_cast< QImage >( imageData );
            else if( imageData.canConvert<QPixmap>() )
                image = qvariant_cast< QPixmap >( imageData ).toImage();
        }
    }

    if( image.isNull() )
        return false;

    // Create a base white canvas (minimum 800x600, or padded if image is larger)
    int bgWidth = qMax( 800, image.width() + 100 );
    int bgHeight = qMax( 600, image.height() + 100 );
    QImage bgImage( bgWidth, bgHeight, QImage::Format_ARGB32 );
    bgImage.fill( Qt::white );

    //auto* view = new QImageView( this );
    //if( !view->openImage( bgImage, tr( "클립보드 이미지" ) ) )
    //{
    //    delete view;
    //    return false;
    //}

    //applyThemeToView( view );
    //addViewTab( view );

    //// Start composite mode with the clipboard image
    //view->startComposite( image );

    return true;
}

bool MainWindow::confirmOpenCapturedImage() const
{
    QMessageBox box( QMessageBox::Question,
                    tr( "캡쳐 결과 열기" ),
                    tr( "캡쳐 이미지가 클립보드에 복사되었습니다." ),
                    QMessageBox::NoButton,
                    const_cast< MainWindow* >( this ) );
    box.setInformativeText( tr( "캡쳐 결과를 새 이미지 뷰어 탭으로 여시겠습니까?" ) );
    box.setDetailedText( tr( "아니요를 선택해도 캡쳐 이미지는 클립보드에 유지됩니다." ) );
    auto* openButton = box.addButton( tr( "이미지 뷰어로 열기" ), QMessageBox::AcceptRole );
    auto* keepClipboardButton = box.addButton( tr( "클립보드에만 복사" ), QMessageBox::RejectRole );
    box.setDefaultButton( openButton );
    box.setEscapeButton( keepClipboardButton );
    box.exec();
    return box.clickedButton() == openButton;
}

bool MainWindow::openCapturedImage( const QImage& image, const QString& title )
{
    if( image.isNull() )
        return false;

    //auto* view = new QImageView( this );
    //applyPersistedViewSettings( view );
    //if( !view->openImage( image, title.isEmpty() ? tr( "캡쳐 이미지" ) : title ) )
    //{
    //    delete view;
    //    return false;
    //}

    //applyThemeToView( view );
    //addViewTab( view );
    return true;
}

// ═══════════════════════════════════════════════════════════
// 슬롯
// ═══════════════════════════════════════════════════════════
void MainWindow::onFileNew()
{
    NewFileTarget target = NewFileTarget::Text;
    if( !promptNewFileTarget( this, &target ) )
        return;

    //if( target == NewFileTarget::Text )
    //{
    //    auto* view = new QTextView( this );
    //    applyPersistedViewSettings( view );
    //    applyThemeToView( view );
    //    addViewTab( view );
    //    return;
    //}

    //auto* view = new QImageView( this );
    //applyPersistedViewSettings( view );
    //if( !view->openBlankCanvas() )
    //{
    //    QMessageBox::warning( this, tr( "오류" ), tr( "빈 캔버스를 만들 수 없습니다." ) );
    //    delete view;
    //    return;
    //}

    //applyThemeToView( view );
    //addViewTab( view );
}

void MainWindow::onFileOpen()
{
    const QStringList files = QFileDialog::getOpenFileNames( this,
        tr( "파일 열기" ), {},
        tr( "모든 지원 파일 (*.pdf *.jpg *.jpeg *.png *.bmp *.gif *.tiff *.tif *.ico *.webp *.svg "
            "*.txt *.log *.ini *.cfg *.xml *.json *.html *.htm *.css *.js *.ts "
            "*.cpp *.c *.h *.hpp *.py *.java *.md *.markdown);;"
            "PDF (*.pdf);;"
            "이미지 (*.jpg *.jpeg *.png *.bmp *.gif *.tiff *.tif *.ico *.webp *.svg);;"
            "텍스트 (*.txt *.log *.ini *.cfg *.xml *.json *.html *.css *.js *.cpp *.c *.h *.py);;"
            "마크다운 (*.md *.markdown);;"
            "모든 파일 (*)" )
    );
    for( const auto& f : files )
        openFile( f );
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
    //if( auto* textView = qobject_cast< QTextView* >( view ) )
    //    started = saveAs ? textView->saveFileAs() : textView->saveFile( {} );
    //else if( auto* markdownView = qobject_cast< QMarkdownView* >( view ) )
    //    started = saveAs ? markdownView->saveFileAs() : markdownView->saveFile( {} );
    //else if( auto* imageView = qobject_cast< QImageView* >( view ) )
    //    started = saveAs ? imageView->saveFileAs() : imageView->saveFile( {} );
    //else if( auto* pdfView = qobject_cast< QPDFView* >( view ) )
    //    started = saveAs ? pdfView->saveFileAs() : pdfView->saveFile( {} );
    //else
    //    started = view->saveFile();

    updateSaveActionState();
    return started;
}

void MainWindow::onCopy()
{
    //if( auto* v = currentView() )
    //    v->copySelection();

    //updateCopyActionState();
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
    if( !canPasteClipboardImage() )
        return;

    //QBaseView* view = currentView();
    //if( auto* imageView = qobject_cast< QImageView* >( view ) )
    //{
    //    const QClipboard* clipboard = QApplication::clipboard();
    //    QImage image = clipboard->image();
    //    if( image.isNull() )
    //    {
    //        const QMimeData* mimeData = clipboard->mimeData();
    //        if( mimeData && mimeData->hasImage() )
    //        {
    //            const QVariant imageData = mimeData->imageData();
    //            if( imageData.canConvert<QImage>() )
    //                image = qvariant_cast< QImage >( imageData );
    //            else if( imageData.canConvert<QPixmap>() )
    //                image = qvariant_cast< QPixmap >( imageData ).toImage();
    //        }
    //    }

    //    if( !image.isNull() )
    //    {
    //        QMessageBox box( QMessageBox::Question,
    //                        tr( "클립보드 이미지 붙여넣기" ),
    //                        tr( "클립보드에 이미지 데이터가 있습니다.\n이미지를 어떻게 처리하시겠습니까?" ),
    //                        QMessageBox::NoButton,
    //                        this );
    //        auto* newTabBtn = box.addButton( tr( "새 탭으로 열기" ), QMessageBox::AcceptRole );
    //        auto* compositeBtn = box.addButton( tr( "현재 탭에 합성" ), QMessageBox::ActionRole );
    //        auto* cancelBtn = box.addButton( tr( "취소" ), QMessageBox::RejectRole );
    //        box.setDefaultButton( compositeBtn );
    //        box.setEscapeButton( cancelBtn );
    //        box.exec();

    //        if( box.clickedButton() == cancelBtn )
    //            return;

    //        if( box.clickedButton() == compositeBtn )
    //        {
    //            imageView->startComposite( image );
    //            return;
    //        }
    //    }
    //}
    //else if( shouldConfirmClipboardImageOpen() && !confirmOpenClipboardImage() )
    //{
    //    return;
    //}

    openClipboardImage();
}

void MainWindow::onResetClipboardImagePastePrompt()
{
    saveClipboardImagePastePolicy( ClipboardImagePastePolicy::Ask );

    if( m_statusLabel )
    {
        statusBar()->showMessage(
            tr( "클립보드 이미지 붙여넣기 동작이 '%1'(으)로 재설정되었습니다." )
                .arg( clipboardImagePastePolicyText( ClipboardImagePastePolicy::Ask ) ),
            4000 );
    }
}

void MainWindow::onCloseTab( int index )
{
    if( index < 0 || !m_tabWidget || index >= m_tabWidget->count() ) return;

    //QWidget* widget = m_tabWidget->widget( index );
    //if( auto* view = qobject_cast< QBaseView* >( widget ) )
    //{
    //    if( m_activeViewLoads.contains( view ) )
    //    {
    //        cancelLoadingView( view, false );
    //        return;
    //    }
    //    if( view->isModified() && canCloseWithTextHotExit( view ) )
    //    {
    //        if( auto* textView = qobject_cast< QTextView* >( view ) )
    //            textView->flushHotExitBackup();
    //    }
    //    else if( view->isModified() )
    //    {
    //        auto btn = QMessageBox::question( this, tr( "저장 확인" ),
    //            tr( "변경사항이 있습니다. 저장하시겠습니까?" ),
    //            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel );
    //        if( btn == QMessageBox::Cancel ) return;
    //        if( btn == QMessageBox::Yes )
    //        {
    //            if( !saveView( view, true ) )
    //                return;
    //            if( view->isLoading() )
    //            {
    //                statusBar()->showMessage( tr( "저장이 진행 중입니다. 저장 완료 후 다시 탭을 닫아 주세요." ), 3000 );
    //                refreshCurrentViewUi();
    //                return;
    //            }
    //        }
    //    }
    //    teardownView( view );
    //}
    //else
    //{
    //    const QSignalBlocker blocker( m_tabWidget );
    //    m_tabWidget->removeTab( index );
    //    widget->deleteLater();
    //}

    refreshCurrentViewUi();
}

void MainWindow::closeEvent( QCloseEvent* event )
{
    //if( m_tabWidget )
    //{
    //    for( int i = 0; i < m_tabWidget->count(); ++i )
    //    {
    //        QWidget* widget = m_tabWidget->widget( i );
    //        if( auto* view = qobject_cast< QBaseView* >( widget ) )
    //        {
    //            if( view->isModified() && canCloseWithTextHotExit( view ) )
    //            {
    //                if( auto* textView = qobject_cast< QTextView* >( view ) )
    //                    textView->flushHotExitBackup();
    //            }
    //            else if( view->isModified() )
    //            {
    //                m_tabWidget->setCurrentIndex( i );
    //                auto btn = QMessageBox::question( this, tr( "저장 확인" ),
    //                    tr( "변경사항이 있습니다. 저장하시겠습니까?\n%1" ).arg( QFileInfo( view->currentFilePath() ).fileName() ),
    //                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel );
    //                if( btn == QMessageBox::Cancel )
    //                {
    //                    event->ignore();
    //                    return;
    //                }
    //                if( btn == QMessageBox::Yes )
    //                {
    //                    if( !saveView( view, true ) )
    //                    {
    //                        event->ignore();
    //                        return;
    //                    }
    //                }
    //            }
    //        }
    //    }
    //}

    shutdownUi();
    QMainWindow::closeEvent( event );
}

void MainWindow::onTabChanged( int /*index*/ )
{
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

void MainWindow::onSettings()
{
    QSettingsDialog dlg( this );
    connect( &dlg, &QSettingsDialog::settingsApplied, this, [this] {
        // 단축키 즉시 적용
        const auto shortcuts = QSettingsDialog::LoadShortcutsFromSettings();
        QSettingsDialog::ApplyShortcutsToActions( shortcuts, this );
        // 열려있는 뷰어에 변경된 설정 적용
        applySettingsToAllViews();
    } );
    dlg.exec();
}

void MainWindow::updateTitle()
{
    auto* v = currentView();
    if( v )
        setWindowTitle( QStringLiteral( "iMonFTS Multi Viewer — %1" ).arg( v->title() ) );
    else
        setWindowTitle( tr( "iMonFTS Multi Viewer" ) );
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
    //else if( auto* tv = qobject_cast< QTextView* >( v ) )
    //{
    //    const QString language = tv->currentLanguage().isEmpty() ? tr( "None" ) : tv->currentLanguage();
    //    const int selectionLength = tv->selectedCharacterCount();
    //    const QString modeText = tv->isReadOnly() ? tr( "읽기 전용" ) : tr( "편집 가능" );
    //    const QString loadModeText = tv->contentLoadModeText();
    //    const QString whitespaceText = tv->isWhitespaceVisible()
    //        ? tr( "제어문자 표시" )
    //        : tr( "제어문자 숨김" );
    //    const QString textStats = selectionLength > 0
    //        ? tr( "문자 %1 | 선택 %2" ).arg( tv->characterCount() ).arg( selectionLength )
    //        : tr( "문자 %1" ).arg( tv->characterCount() );
    //    const QString eolText = [&] {
    //        switch( tv->detectedLineEnding() )
    //        {
    //            case QTextView::LF: return QStringLiteral( "LF" );
    //            case QTextView::CR: return QStringLiteral( "CR" );
    //            default: return QStringLiteral( "CRLF" );
    //        }
    //        }( );
    //    const QString indentText = tv->useTabs()
    //        ? tr( "탭 %1" ).arg( tv->tabWidth() )
    //        : tr( "공백 %1" ).arg( tv->tabWidth() );
    //    info = tr( "줄 %1/%2 | 열 %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10 | %11" )
    //        .arg( tv->currentLine() )
    //        .arg( tv->lineCount() )
    //        .arg( tv->currentColumn() )
    //        .arg( modeText )
    //        .arg( loadModeText )
    //        .arg( textStats )
    //        .arg( indentText )
    //        .arg( whitespaceText )
    //        .arg( eolText )
    //        .arg( tv->currentEncodingDisplayName() )
    //        .arg( language );
    //}
    //else if( qobject_cast< QMarkdownView* >( v ) )
    //{
    //    info = tr( "Markdown" );
    //}
    m_statusLabel->setText( info );
}

void MainWindow::updateCopyActionState()
{
    //if( m_copyAction )
    //    m_copyAction->setEnabled( currentView() && currentView()->canCopy() );
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
        m_pasteAction->setEnabled( canPasteClipboardImage() );
}

// ═══════════════════════════════════════════════════════════
// 드래그 앤 드롭
// ═══════════════════════════════════════════════════════════
bool MainWindow::eventFilter( QObject* watched, QEvent* event )
{
    if( !event )
        return QMainWindow::eventFilter( watched, event );

    const QEvent::Type type = event->type();
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
        statusBar()->showMessage( tr( "드롭한 항목 %1개를 처리했습니다." ).arg( openedRequests ), 2500 );
}

void MainWindow::openDroppedDirectory( const QString& dirPath )
{
    const QString firstImage = firstImageFileInDirectory( dirPath );
    if( !firstImage.isEmpty() )
    {
        openFile( firstImage );
        return;
    }

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

QString MainWindow::firstImageFileInDirectory( const QString& dirPath ) const
{
    QDir dir( dirPath );
    QStringList filters;
    for( const QString& ext : imageFileExtensions() )
        filters << QStringLiteral( "*.%1" ).arg( ext );

    dir.setNameFilters( filters );
    const QFileInfoList images = dir.entryInfoList( QDir::Files, QDir::Name | QDir::IgnoreCase );
    if( images.isEmpty() )
        return {};

    return images.first().absoluteFilePath();
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

    qDebug().noquote() << "[MainWindow] shutdownUi end" << this;
}

//QStringList imageFileExtensions()
//{
//    return { "jpg", "jpeg", "png", "bmp", "gif", "tiff", "tif", "ico", "webp", "svg" };
//}
//
//QStringList markdownFileExtensions()
//{
//    return { "md", "markdown", "mdown" };
//}
//
//MainWindow::MainWindow( QWidget* parent )
//    : QMainWindow( parent )
//{
//    Ui.setupUi( this );
//
//    //Ui.tabEditor->InitializeEmpty();
//    //connect( Ui.tabEditor, &EditorTabWidget::activeEditorChanged, this, [this]( QBaseEditor* editor ) {
//    //    if( editor == nullptr )
//    //    {
//    //        statusBar()->clearMessage();
//    //        return;
//    //    }
//
//    //    statusBar()->showMessage( tr( "Active editor: %1" ).arg( QDir::toNativeSeparators( editor->FilePath() ) ) );
//    //} );
//    //connect( Ui.tabEditor, &EditorTabWidget::fileOpenFailed, this, [this]( const QString& filePath, const QString& reason ) {
//    //    QMessageBox::warning( this, tr( "Open File" ), tr( "Failed to open '%1'.\n%2" ).arg( QDir::toNativeSeparators( filePath ), reason ) );
//    //} );
//    //Ui.tabEditor->SetRulerVisibleForAllEditors( QSettingsDialog::IsTextViewerRulerWidgetVisible() );
//
//    //// editorTabs_->setTabsClosable(true);
//    //// editorTabs_->setMovable(true);
//
//
//    createMenus();
//}
//
//MainWindow::~MainWindow() = default;
//
//void MainWindow::OpenFile( const QString& FilePath )
//{
//    const QString normalizedPath = normalizeFilePath( FilePath );
//    if( normalizedPath.isEmpty() ) 
//        return;
//
//    for( int i = 0; i < Ui.tabEditor->count(); ++i )
//    {
//        auto* view = dynamic_cast< QBaseView* >( Ui.tabEditor->widget( i ) );
//        if( view && normalizeFilePath( view->currentFilePath() ) == normalizedPath )
//        {
//            Ui.tabEditor->setCurrentIndex( i );
//            return;
//        }
//    }
//
//    //if( shouldConfirmBinaryTextOpen( normalizedPath ) && !confirmOpenBinaryTextFile( normalizedPath ) )
//    //    return;
//
//    //QBaseView* view = createViewForFile( normalizedPath );
//    //if( !view )
//    //{
//    //    QMessageBox::warning( this, tr( "오류" ),
//    //                         tr( "지원하지 않는 파일 형식입니다:\n%1" ).arg( normalizedPath ) );
//    //    return;
//    //}
//
//    //connect( view, &QBaseView::fileOpened, this, [this]( const QString& openedPath ) {
//    //    const QString normalizedOpenedPath = normalizeFilePath( openedPath );
//    //    if( !normalizedOpenedPath.isEmpty() )
//    //        addRecentFile( normalizedOpenedPath );
//    //} );
//    //connect( view, &QBaseView::fileOpenFailed, this,
//    //        [this, view]( const QString& failedPath, const QString& errorMessage ) {
//    //            const QString effectivePath = failedPath.isEmpty()
//    //                ? view->currentFilePath()
//    //                : failedPath;
//    //            const QString effectiveMessage = errorMessage.isEmpty()
//    //                ? tr( "파일을 열 수 없습니다:\n%1" ).arg( effectivePath )
//    //                : errorMessage;
//
//    //            statusBar()->showMessage( effectiveMessage, 4000 );
//    //            QMessageBox::warning( this, tr( "오류" ), effectiveMessage );
//
//    //            teardownView( view );
//    //            refreshCurrentViewUi();
//    //        } );
//
//    //connectViewStatusSignals( view );
//
//    //// 파일 열기 전에 설정에서 기본값 적용 (뷰 모드, 글꼴 등)
//    //applyPersistedViewSettings( view );
//
//    //const bool asyncOpen = view->opensFileAsynchronously();
//    //if( asyncOpen )
//    //{
//    //    applyThemeToView( view );
//    //    view->setWatermark( m_globalWatermark );
//    //}
//
//    //if( !view->openFile( normalizedPath ) )
//    //{
//    //    if( asyncOpen )
//    //    {
//    //        ViewTeardownOptions teardownOptions;
//    //        teardownOptions.deleteLater = false;
//    //        teardownView( view, teardownOptions );
//    //        delete view;
//    //        refreshCurrentViewUi();
//    //        return;
//    //    }
//
//    //    QMessageBox::warning( this, tr( "오류" ),
//    //                         tr( "파일을 열 수 없습니다:\n%1" ).arg( normalizedPath ) );
//    //    delete view;
//    //    return;
//    //}
//
//    //if( asyncOpen )
//    //{
//    //    addViewTab( view );
//    //}
//    //else
//    //{
//    //    applyThemeToView( view );
//    //    view->setWatermark( m_globalWatermark );
//    //    addViewTab( view );
//    //}
//}
//
//void MainWindow::onFileNew()
//{
//}
//
//void MainWindow::onFileOpen()
//{
//    const QStringList files = QFileDialog::getOpenFileNames( this,
//        tr( "파일 열기" ), {},
//        tr( "모든 지원 파일 (*.jpg *.jpeg *.png *.bmp *.gif *.tiff *.tif *.ico *.webp *.svg "
//            "*.rst *.py *.md *.markdown);;"
//            "이미지 (*.jpg *.jpeg *.png *.bmp *.gif *.tiff *.tif *.ico *.webp *.svg);;"
//            "텍스트 (*.rst *.py);;"
//            "마크다운 (*.md *.markdown);;"
//            "모든 파일 (*)" )
//    );
//    
//    for( const auto& f : files )
//        OpenFile( f );
//}
//
//void MainWindow::onFileSave()
//{
//}
//
//void MainWindow::onFileSaveAs()
//{
//}
//
//void MainWindow::onSettings()
//{
//    //QSettingsDialog dlg(this);
//    //connect( &dlg, &QSettingsDialog::settingsApplied, this, [this] {
//    //    Ui.tabEditor->SetRulerVisibleForAllEditors( QSettingsDialog::IsTextViewerRulerWidgetVisible() );
//    //} );
//    // connect(&dlg, &QSettingsDialog::settingsApplied, this, [this] {
//    //     // 단축키 즉시 적용
//    //     const auto shortcuts = QSettingsDialog::loadShortcutsFromSettings();
//    //     QSettingsDialog::applyShortcutsToActions(shortcuts, this);
//    //     // 열려있는 뷰어에 변경된 설정 적용
//    //     applySettingsToAllViews();
//    // });
//    //if( dlg.exec() == QDialog::Accepted )
//    //    Ui.tabEditor->SetRulerVisibleForAllEditors( QSettingsDialog::IsTextViewerRulerWidgetVisible() );
//}
//
//void MainWindow::createMenus()
//{
//    auto* fileMenu = menuBar()->addMenu( tr( "파일(&F)" ) );
//
//    m_newAction = fileMenu->addAction( tr( "새 파일(&N)" ), this, &MainWindow::onFileNew );
//    m_newAction->setObjectName( QStringLiteral( "file.new" ) );
//    m_newAction->setProperty( "mv.shortcutId", QStringLiteral( "file.new" ) );
//    m_newAction->setShortcut( QKeySequence::New );
//    m_newAction->setShortcutContext( Qt::ApplicationShortcut );
//
//    m_openAction = fileMenu->addAction( tr( "열기(&O)..." ), QKeySequence::Open, this, &MainWindow::onFileOpen );
//    m_openAction->setObjectName( QStringLiteral( "file.open" ) );
//
//    m_saveAction = fileMenu->addAction( tr( "저장(&S)" ), this, &MainWindow::onFileSave );
//    m_saveAction->setObjectName( QStringLiteral( "file.save" ) );
//    m_saveAction->setProperty( "mv.shortcutId", QStringLiteral( "file.save" ) );
//    m_saveAction->setShortcut( QKeySequence::Save );
//    m_saveAction->setShortcutContext( Qt::ApplicationShortcut );
//
//    m_saveAsAction = fileMenu->addAction( tr( "다른 이름으로 저장(&A)..." ), this, &MainWindow::onFileSaveAs );
//    m_saveAsAction->setObjectName( QStringLiteral( "file.saveAs" ) );
//    m_saveAsAction->setProperty( "mv.shortcutId", QStringLiteral( "file.saveAs" ) );
//    m_saveAsAction->setShortcut( QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_S ) );
//    m_saveAsAction->setShortcutContext( Qt::ApplicationShortcut );
//
//    m_recentMenu = fileMenu->addMenu( tr( "최근 파일(&R)" ) );
//
//    fileMenu->addSeparator();
//
//    fileMenu->addAction( tr( "종료(&X)" ), QKeySequence::Quit,
//                    QApplication::instance(), &QApplication::quit );
//
//    auto* editMenu = menuBar()->addMenu( tr( "편집(&E)" ) );
//
//    auto* settingsMenu = menuBar()->addMenu(tr("설정(&S)"));
//    auto* settingsAction = settingsMenu->addAction(tr("설정(&I)..."), this, &MainWindow::onSettings);
//    settingsAction->setObjectName(QStringLiteral("app.settings"));
//    settingsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
//    settingsAction->setShortcutContext(Qt::ApplicationShortcut);
//
//}
//
//int MainWindow::addViewTab( QBaseView* view )
//{
//    return 0;
//}
//
//void MainWindow::applyThemeToView( QBaseView* View ) const
//{
//    if( !View )
//        return;
//
//    const auto theme = ThemeManager::instance().currentTheme() == ThemeManager::Dark
//        ? QBaseView::Theme::Dark
//        : QBaseView::Theme::Light;
//
//    View->setTheme( theme );
//}
//
//QString MainWindow::normalizeFilePath( const QString& FilePath )
//{
//    const QFileInfo info( FilePath );
//    const QString canonical = info.canonicalFilePath();
//    return QDir::cleanPath( canonical.isEmpty() ? info.absoluteFilePath() : canonical );
//}
//
//QBaseView* MainWindow::createViewForFile( const QString& filePath )
//{
//    const QString ext = QFileInfo( filePath ).suffix().toLower();
//    
//    //if( imageFileExtensions().contains( ext ) )
//    //    return new QImageView( this );
//
//    //if( markdownFileExtensions().contains( ext ) )
//    //    return new QMarkdownView( this );
//
//    // return new QBaseEditor( this );
//    return {};
//}
