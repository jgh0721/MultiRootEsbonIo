#include "stdafx.h"
#include "MainWindow.hpp"

#include "core/solAppSettings.hpp"
#include "core/solBaseView.hpp"
#include "core/solExternalChangeWatcher.hpp"
#include "core/solFileKinds.hpp"
#include "core/solPythonEnvMgr.hpp"
#include "core/solRecentItems.hpp"
#include "core/solRestWorkspaceController.hpp"
#include "core/solRstPathIndex.hpp"
#include "core/solSettingsWriter.hpp"
#include "core/solSphinxDiagnosticsStore.hpp"
#include "core/solWorkspaceSearch.hpp"
#include "core/solWorkspaceSession.hpp"
#include "core/solThemeManager.hpp"
#include "core/solShadowBackupStore.hpp"
#include "core/solUpdateService.hpp"
#include "editor/QBaseEditor.hpp"
#include "core/solSphinxProjectRegistry.hpp"
#include "uis/dlgAbout.hpp"
#include "uis/dlgSettings.hpp"
#include "uis/dlgSphinxBuild.hpp"
#include "uis/FileTreeFilterProxy.hpp"
#include "uis/PanelActionIcons.hpp"
#include "uis/QuickOpenDialog.hpp"
#include "uis/TabSwitcherPopup.hpp"
#include "utils/DwmTitleBar.hpp"
#include "utils/solBackgroundWork.hpp"
#include "utils/solPhaseTrace.hpp"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockSplitter.h>
#include <DockWidget.h>

#include <QWebEnginePage>
#include <QWebEngineScriptCollection>

#include <QActionGroup>
#include <QMenuBar>
#include <QScopeGuard>
#include <QStatusBar>
#include <QStyle>
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
#include <QPropertyAnimation>
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
#include <functional>
#include <limits>

namespace
{
    constexpr auto kPreviewZoomProperty = "mrst_previewZoomPercent";

    bool isMarkdownView( const QBaseView* view )
    {
        const auto* textView = qobject_cast< const QTextView* >( view );
        return textView != nullptr
               && mrst::filekinds::hasExtension( textView->currentFilePath(),
                                                 mrst::filekinds::markdownExtensions() );
    }

    bool isPreviewDocumentView( const QBaseView* view )
    {
        const auto* textView = qobject_cast< const QTextView* >( view );
        if( textView == nullptr )
            return false;

        const QString path = textView->currentFilePath();
        return mrst::filekinds::hasExtension( path, mrst::filekinds::markdownExtensions() )
               || mrst::filekinds::hasExtension(
                   path, mrst::filekinds::restructuredTextExtensions() );
    }

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

    /// **앱을 닫을 때** 이 뷰의 저장 확인을 건너뛰어도 되는가.
    ///
    /// hot exit 는 종료에만 걸린다. 탭 하나를 닫는 것은 "이 문서를 버린다" 는
    /// 뜻일 수 있으므로 거기서는 그대로 묻는다(onCloseTab).
    ///
    /// 제한 프리뷰와 절단 문서는 편집기가 파일의 일부만 들고 있다. 그 상태를
    /// 스냅샷으로 떠서 되살리면 잘려 나간 뒤쪽이 원본에서 사라진 것처럼 보이므로
    /// 빼고, 평소처럼 저장 여부를 묻는다.
    bool canCloseWithTextHotExit( QBaseView* view )
    {
        auto* textView = qobject_cast< QTextView* >( view );
        return textView
            && textView->isHotExitEnabled()
            && !textView->isLimitedPreviewMode()
            && !textView->isContentTruncated();
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

    /// refresh 가 false 면 레이아웃 무효화를 하지 않는다. 호출자가 뒤에서 한 번에
    /// 모아 돌릴 때 쓴다(updateViewerToolBar 의 스코프 가드).
    void destroyViewerToolBar( QWidget* host, QPointer<QToolBar>& toolBar, bool refresh = true )
    {
        if( !host || !toolBar )
            return;

        QToolBar* oldToolBar = toolBar;
        toolBar = nullptr;

        if( auto* layout = host->layout() )
            layout->removeWidget( oldToolBar );

        oldToolBar->hide();
        delete oldToolBar;
        if( refresh )
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

    /// 외부 변경을 알리는 상태 표시줄 메시지가 머무는 시간. 자동 불러오기는
    /// 조용히 일어나므로, 방금 화면이 바뀐 이유를 읽을 시간은 있어야 한다.
    constexpr int kExternalChangeStatusMs = 5000;
    /// 뷰가 읽기/쓰기 중이라 지금 다시 불러올 수 없을 때 다시 시도하는 간격.
    constexpr int kExternalReloadRetryMs = 500;

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
    // 지난 워크스페이스의 창 크기·위치가 있으면 위 기본값을 덮는다. **show()
    // 보다 앞이어야** 창이 1024x768 로 한 번 떴다가 커지는 것이 보이지 않는다.
    // 도크와 스플리터는 탭이 다 열린 뒤라야 의미가 있어 여기서 하지 않는다
    // (applySessionLayout).
    restoreWindowGeometryForLastWorkspace();
    setAcceptDrops( true );
    if( auto* app = QCoreApplication::instance() )
        app->installEventFilter( this );

    startStallWatchdog();

    setupCentralContainer();
    // .ui 가 세로로 늘어놓은 pnl* 패널들을 도크로 옮긴다. 이 뒤로는 Ui.pnl* 을
    // 직접 건드리지 않는다 — 부모가 도크 위젯이다.
    setupDockLayout();

    setupExplorerPanel();

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

    // 편집기 | 프리뷰 스플리터.
    // 자식 하나가 QWebEngineView(별도 합성 표면)라 핸들을 끌면 노출 영역이
    // 실시간으로 바뀐다. 두 프레임이 스스로 배경을 칠하게 해야 그 틈이
    // 이전 픽셀(검은 띠)로 남지 않는다.
    // 양쪽 다 핸들을 끝까지 끌어 완전히 접을 수 있다.
    //
    // 예전에는 setChildrenCollapsible(false) 였다. 그 값의 본래 목적은 프리뷰가
    // 0 폭으로 **시작**하는 것을 막는 것이었는데, 그 일은 지금
    // ensureVisiblePreviewSplit() 이 하고 있어서 접기를 막을 이유가 남지 않았다.
    // 반대로 대가는 컸다 — 프리뷰만 보고 싶을 때 편집기를 치울 수 없었다.
    Ui.splitter_2->setChildrenCollapsible( true );
    Ui.splitter_2->setStretchFactor( 0, 1 );
    Ui.splitter_2->setStretchFactor( 1, 1 );
    // 최소 폭을 양쪽에 준다. 접기와 어긋나지 않는다 — QSplitter 는 접을 수 있는
    // 자식을 이 하한 **아래로 끌면 0 으로 스냅**하고, 그 사이 값에는 두지 않는다.
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

    // ── 상태표시줄 왼쪽: 무슨 일이 얼마나 되었는가 ──
    //
    // addWidget 은 왼쪽, addPermanentWidget 은 오른쪽이다. 진행 상황은 왼쪽에 둔다 —
    // 오른쪽은 줄·열·인코딩처럼 **가만히 있는** 값의 자리이고, 그 사이에 끼우면
    // 진행 중에만 나타나는 위젯 때문에 옆 칩들이 매번 좌우로 밀린다.
    //
    // ⚠ 이 셋은 **창의 최소 폭을 요구하면 안 된다.**
    //
    // QStatusBar 는 QBoxLayout 이고, 그 안의 위젯이 요구하는 최소 폭은 그대로
    // 창의 최소 폭이 된다. 셋 다 평소에는 숨어 있어(레이아웃에서 빠진다) 아무
    // 영향이 없지만, 빌드가 시작되어 **나타나는 순간** 창의 최소 폭이 그만큼
    // 커지고 Qt 가 창을 강제로 넓힌다. 표시가 사라져도 창은 넓어진 채 남는다 —
    // 최소 폭이 줄었다고 창을 다시 좁혀 주지는 않는다.
    //
    // 실측(1773줄 .rst, 저장 한 번): 창이 1413 → 1609 로 자랐다. 그 순간 편집기
    // 폭이 달라지면서 자동 줄넘김이 다시 계산되고, 보고 있던 줄의 화면 위치가
    // 통째로 흔들린다. 저장할 때마다 반복된다.
    statusMessageLabel_ = new QLabel( this );
    statusMessageLabel_->setVisible( false );
    // 남는 자리에 맞춘다. 긴 문구가 창을 밀지 않는다.
    statusMessageLabel_->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Preferred );
    statusProgressBar_ = new QProgressBar( this );
    statusProgressBar_->setVisible( false );
    statusProgressBar_->setTextVisible( true );
    statusProgressBar_->setAlignment( Qt::AlignCenter );
    statusProgressBar_->setFormat( QStringLiteral( "%p%" ) );
    // setFixedWidth 는 최소 폭도 180 으로 못 박는다. 자리가 있으면 180, 없으면
    // 줄어들도록 상한만 준다.
    statusProgressBar_->setMinimumWidth( 0 );
    statusProgressBar_->setMaximumWidth( 180 );
    statusCancelButton_ = new QPushButton( tr( "취소" ), this );
    statusCancelButton_->setVisible( false );
    statusCancelButton_->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Fixed );
    statusCancelButton_->setAutoDefault( false );
    statusCancelButton_->setDefault( false );
    connect( statusCancelButton_, &QPushButton::clicked, this, &MainWindow::onCancelLoading );
    statusBar()->addWidget( statusMessageLabel_ );
    statusBar()->addWidget( statusProgressBar_ );
    statusBar()->addWidget( statusCancelButton_ );

    transientStatusTimer_ = new QTimer( this );
    transientStatusTimer_->setSingleShot( true );
    connect( transientStatusTimer_, &QTimer::timeout, this, &MainWindow::clearTransientStatus );

    m_statusLabel = new QLabel( this );
    statusBar()->addPermanentWidget( m_statusLabel );
    showTransientStatus( tr( "Ready" ) );

    m_loadingAnimationTimer = new QTimer( this );
    m_loadingAnimationTimer->setInterval( kLoadingAnimationIntervalMs );
    connect( m_loadingAnimationTimer, &QTimer::timeout, this, &MainWindow::advanceLoadingAnimation );

    // 최근 파일 / 워크스페이스 복원
    AppSettings settings;
    m_recentFiles = settings.value( "recentFiles" ).toStringList();
    m_recentWorkspaces = settings.value( "recentWorkspaces" ).toStringList();
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
    applyConfiguredFonts();

    // 외부 파일 편집 인식. 지금은 감시할 파일이 없어 타이머도 스레드도 뜨지
    // 않는다 — 탭이 열리면서 refreshExternalWatchSet() 이 채운다.
    externalWatcher_ = new mrst::ExternalChangeWatcher( this );
    connect( externalWatcher_, &mrst::ExternalChangeWatcher::sigFileChanged,
            this, &MainWindow::onExternalFileChanged );
    connect( externalWatcher_, &mrst::ExternalChangeWatcher::sigFileVanished,
            this, &MainWindow::onExternalFileVanished );

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
    // 도크도 같은 이유로 여기서 비율을 정한다. 세션이 배치를 복원했으면
    // applyDefaultDockSizes() 가 스스로 물러난다.
    QTimer::singleShot( 0, this, &MainWindow::applyDefaultDockSizes );
    // 탭이 다 열린 뒤에 포커스를 문서로 보낸다. 이것이 없으면 Qt 가 탭 순서의
    // 첫 위젯(지금은 탐색기 필터칸)에 포커스를 주어, 창이 뜨자마자 친 글자가
    // 문서가 아니라 필터로 들어간다.
    QTimer::singleShot( 0, this, &MainWindow::focusActiveEditor );
}

void MainWindow::focusActiveEditor()
{
    if( m_shuttingDown )
        return;
    // 프리뷰 전체 화면에서는 프리뷰가 포커스를 쥐고 있어야 PageDown 으로 읽어
    // 내려갈 수 있다. 그 상태를 여기서 뺏지 않는다.
    if( previewFullScreen_.active )
        return;

    if( QBaseView* view = currentView(); view != nullptr )
    {
        view->focusContent();
        return;
    }

    // 열린 문서가 없다. 그래도 필터칸에 두지는 않는다 — 글자를 입력하는 칸이라
    // 무심코 친 키가 문서를 찾는 대신 필터를 채운다. 트리가 자연스러운 자리다.
    if( Ui.treLeftSideFolterTree != nullptr )
        Ui.treLeftSideFolterTree->setFocus( Qt::OtherFocusReason );
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

    showPreviewStartPage();

    // 생성자의 applyCurrentTheme() 는 page() 가 없어 바탕색을 못 칠했다. 지금 칠한다.
    Ui.webEngineView->page()->setBackgroundColor( ThemeManager::instance().backgroundColor() );

    if( controller_ )
        controller_->setPreviewView( Ui.webEngineView );
}

void MainWindow::showPreviewStartPage()
{
    if( !previewInitialised_ || Ui.webEngineView == nullptr )
        return;

    Ui.webEngineView->stop();
    //: 문서를 열기 전 프리뷰 영역에 보이는 시작 화면. <h1>/<p> 태그는 그대로 둘 것.
    Ui.webEngineView->setHtml( tr( "<h1>MultiRoot reST</h1><p>셸이 시작되었습니다.</p>" ) );
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

    auto* quickOpenAction = fileMenu->addAction( QString(), this, &MainWindow::showQuickOpen );
    quickOpenAction->setObjectName( QStringLiteral( "file.quickOpen" ) );
    quickOpenAction->setProperty( "mv.shortcutId", QStringLiteral( "file.quickOpen" ) );
    quickOpenAction->setShortcut( QKeySequence( Qt::ALT | Qt::SHIFT | Qt::Key_O ) );
    quickOpenAction->setShortcutContext( Qt::ApplicationShortcut );

    auto* openWorkspace = fileMenu->addAction( QString(), QKeySequence::Open, this, &MainWindow::onWorkspaceOpen );
    openWorkspace->setObjectName( QStringLiteral( "file.openWorkspace" ) );
    openWorkspace->setProperty( "mv.shortcutId", QStringLiteral( "file.openWorkspace" ) );
    openWorkspace->setShortcut( QKeySequence::Open );
    openWorkspace->setShortcutContext( Qt::ApplicationShortcut );

    m_closeWorkspaceAction = fileMenu->addAction( QString(), this, &MainWindow::onWorkspaceClose );
    m_closeWorkspaceAction->setObjectName( QStringLiteral( "file.closeWorkspace" ) );
    m_closeWorkspaceAction->setEnabled( false );

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

    // 패널을 되살리는 곳. 도크의 닫기 버튼을 누르면 그 패널이 화면에서 사라지고,
    // 이 메뉴가 없으면 다시 꺼낼 방법이 없다.
    //
    // ADS 의 CDockManager::viewMenu() 를 쓰지 않는다. 제목이 영어 "Show View" 로
    // 박혀 있어 번역 파이프라인을 타지 않는다. 항목 글자는 도크 제목을 그대로
    // 따라오므로(retranslateDockTitles 참고) 여기서 따로 손댈 것이 없다.
    dockPanelsMenu_ = viewMenu->addMenu( QString() );
    dockPanelsMenu_->setObjectName( QStringLiteral( "menu.view.panels" ) );
    for( ads::CDockWidget* dock : { dockExplorer_, dockOutlineDocument_, dockOutlineProject_,
                                    dockDiagnostics_, dockLog_ } )
    {
        if( dock != nullptr )
            dockPanelsMenu_->addAction( dock->toggleViewAction() );
    }
    // 검색은 setupWorkspaceSearchTab() 이 만든다 — createMenus() 보다 뒤다.

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

    previewFullScreenAction_ = viewMenu->addAction( QString(), this,
                                                   &MainWindow::togglePreviewFullScreen );
    previewFullScreenAction_->setObjectName( QStringLiteral( "preview.fullScreen" ) );
    previewFullScreenAction_->setProperty( "mv.shortcutId", QStringLiteral( "preview.fullScreen" ) );
    previewFullScreenAction_->setShortcut( QKeySequence( Qt::Key_F11 ) );
    previewFullScreenAction_->setShortcutContext( Qt::ApplicationShortcut );
    // 체크 상태로 둔다. 전체 화면에서는 메뉴 바가 숨으므로 이 표시를 볼 수
    // 있는 것은 되돌아온 뒤뿐이지만, 그 한 번이 "F11 이 그 토글이었다" 를 알려 준다.
    previewFullScreenAction_->setCheckable( true );

    // 전체 화면에서만 듣는 Esc. 메뉴에 넣지 않는다 — 메뉴 항목으로 보이면
    // 단축키 설정 표에 없는 Id 를 재정의할 수 있다는 착각을 준다.
    previewExitFullScreenAction_ = new QAction( this );
    previewExitFullScreenAction_->setShortcut( QKeySequence( Qt::Key_Escape ) );
    previewExitFullScreenAction_->setShortcutContext( Qt::WindowShortcut );
    previewExitFullScreenAction_->setEnabled( false );
    connect( previewExitFullScreenAction_, &QAction::triggered, this,
            [this] { setPreviewFullScreen( false ); } );
    addAction( previewExitFullScreenAction_ );

    // 탭 목록. 전체 화면에서는 탭 바가 보이지 않으므로 이것이 문서를 옮기는
    // 유일한 수단이 되고, 평소에도 Visual Studio 처럼 직전 문서로 한 번에 간다.
    viewMenu->addSeparator();
    auto* nextTabAction = viewMenu->addAction( QString(), this, [this] { showTabSwitcher( true ); } );
    nextTabAction->setObjectName( QStringLiteral( "tab.next" ) );
    nextTabAction->setProperty( "mv.shortcutId", QStringLiteral( "tab.next" ) );
    nextTabAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_Tab ) );
    nextTabAction->setShortcutContext( Qt::ApplicationShortcut );

    auto* previousTabAction = viewMenu->addAction( QString(), this, [this] { showTabSwitcher( false ); } );
    previousTabAction->setObjectName( QStringLiteral( "tab.previous" ) );
    previousTabAction->setProperty( "mv.shortcutId", QStringLiteral( "tab.previous" ) );
    previousTabAction->setShortcut( QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_Tab ) );
    previousTabAction->setShortcutContext( Qt::ApplicationShortcut );

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

    // 창으로 돌아왔다. 밖에서 편집기를 쓰고 온 직후가 여기다 — OS 알림이 오지
    // 않는 경로(네트워크 드라이브, 컨테이너 안 볼륨)에서는 이것이 유일한 그물이고,
    // 알림이 오는 경로에서도 자리를 비운 동안 모아 둔 질문을 지금 꺼낸다.
    if( event != nullptr && event->type() == QEvent::ActivationChange && isActiveWindow() )
    {
        if( externalWatcher_ != nullptr )
            externalWatcher_->recheckAll();
        flushExternalChangePrompts();
    }

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
    retranslateExplorerPanel();

    if( Ui.edtOutlineDocumentFilter != nullptr )
        Ui.edtOutlineDocumentFilter->setPlaceholderText( tr( "필터 (부분 일치)" ) );
    if( Ui.edtOutlineProjectFilter != nullptr )
        Ui.edtOutlineProjectFilter->setPlaceholderText( tr( "필터 (부분 일치)" ) );

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
    retranslateDockTitles();

    // 뷰어 도구모음은 QTextView 가 통째로 다시 만든다. 편집기 라벨(언어/인코딩/
    // 줄바꿈/탭 간격)이 여기서 한꺼번에 해결된다 — 테마 전환이 이미 같은 경로를
    // 쓰고 있다.
    updateViewerToolBar();

    updateEnvStatusChip();
    if( statusCancelButton_ != nullptr )
        statusCancelButton_->setText( tr( "취소" ) );

    // Ui.retranslateUi() 가 덮어쓴 창 제목을 되돌린다. 순서가 뒤바뀌면 제목
    // 표시줄에 "MainWindow" 가 남는다.
    updateTitle();
}

void MainWindow::retranslateWorkspaceSearchTab()
{
    if( searchTabPage_ == nullptr )
        return;

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
    actionText( "file.quickOpen",     tr( "파일 빠르게 열기(&Q)..." ) );
    actionText( "file.openWorkspace", tr( "워크스페이스 열기(&O)..." ) );
    actionText( "file.closeWorkspace", tr( "워크스페이스 닫기(&W)" ) );
    actionText( "file.save",          tr( "저장(&S)" ) );
    actionText( "file.saveAs",        tr( "다른 이름으로 저장(&A)..." ) );
    menuTitle ( "menu.recent",        tr( "최근 파일/워크스페이스(&R)" ) );
    actionText( "tab.close",          tr( "현재 탭 닫기(&C)" ) );
    actionText( "app.quit",           tr( "종료(&X)" ) );

    menuTitle ( "menu.edit",          tr( "편집(&E)" ) );
    actionText( "edit.copy",          tr( "복사(&C)" ) );
    actionText( "edit.paste",         tr( "붙여넣기(&P)" ) );
    actionText( "editor.completion",  tr( "자동 완성(&M)" ) );

    menuTitle ( "menu.view",          tr( "보기(&V)" ) );
    actionText( "view.themeToggle",   tr( "테마 전환" ) );
    menuTitle ( "menu.view.panels",   tr( "패널(&P)" ) );
    actionText( "editor.foldAll",     tr( "모두 접기(&F)" ) );
    actionText( "editor.unfoldAll",   tr( "모두 펼치기(&U)" ) );
    actionText( "preview.rebuild",    tr( "프리뷰 다시 빌드(&R)" ) );
    actionText( "preview.fullScreen", tr( "프리뷰 전체 화면(&L)" ) );
    actionText( "tab.next",           tr( "다음 탭(&N)" ) );
    actionText( "tab.previous",       tr( "이전 탭(&P)" ) );

    menuTitle ( "menu.settings",      tr( "설정(&S)" ) );
    actionText( "app.settings",       tr( "설정(&I)..." ) );
    actionText( "app.checkUpdate",    tr( "업데이트 확인(&U)..." ) );

    menuTitle ( "menu.help",          tr( "도움말(&H)" ) );
    actionText( "app.about",          tr( "정보(&A)..." ) );
}

// ═══════════════════════════════════════════════════════════
// 중앙 컨테이너 구성
//
// 메뉴 바 바로 아래에 뷰어 도구모음 슬롯을 두고, 그 밑에 도크 매니저를
// 놓는다(setupDockLayout 이 넣는다). 이 슬롯이 없으면 updateViewerToolBar() 가
// 도구모음을 부모 없는 최상위 위젯으로 만들어 별도 창으로 떠 버린다.
// ═══════════════════════════════════════════════════════════
void MainWindow::setupCentralContainer()
{
    // takeCentralWidget() 은 소유권만 넘기고 삭제하지 않는다.
    // setCentralWidget() 은 기존 중앙 위젯을 삭제하므로 반드시 먼저 떼어낸다.
    //
    // 이 위젯은 **레이아웃에 넣지 않는다.** .ui 의 centralwidget 은 이제
    // frmContents 와 pnl* 를 담아 두는 껍데기일 뿐이고, setupDockLayout() 이
    // 그것들을 도크로 옮긴 뒤 껍데기를 지운다. 예전처럼 레이아웃에 넣으면
    // 내용이 빠져나간 빈 위젯이 stretch 1 로 남아 창의 절반을 먹는다.
    m_uiCentralShell = takeCentralWidget();

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

    setCentralWidget( m_centralContainer );
}

// ═══════════════════════════════════════════════════════════
// 도킹 배치 (Qt ADS)
//
// .ui 는 패널 내용만 선언한다 (그 파일 머리의 주석 참고). 배치는 여기서
// 만든다 — 좌측 위에 탐색기, 그 아래에 요약 두 개를 탭으로, 하단에 진단·로그,
// 중앙에 편집기|프리뷰. 지금까지 스플리터가 만들던 그림과 같지만, 이제 각
// 패널에 핀 버튼이 붙어 창 가장자리 탭으로 접힌다.
// ═══════════════════════════════════════════════════════════
ads::CDockWidget* MainWindow::makeDock( const char* id, const QString& title, QWidget* content )
{
    auto* dock = new ads::CDockWidget( dockManager_, title, this );
    // objectName 을 반드시 명시한다. CDockWidget 은 이것을 제목으로 초기화하는데,
    // saveState()/restoreState() 가 이 값을 키로 쓴다. 제목은 번역 대상이므로
    // 그대로 두면 **UI 언어를 바꾼 순간 저장해 둔 배치를 못 읽는다.**
    dock->setObjectName( QLatin1String( id ) );

    if( content != nullptr )
    {
        // 기본값(AutoScrollArea)은 스크롤 영역이 아닌 위젯을 QScrollArea 로
        // 감싼다. pnl* 은 전부 레이아웃만 든 평범한 QWidget 이라 그렇게 감싸이면
        // 스크롤바가 이중으로 생기고, 중앙의 편집기|프리뷰 스플리터는 아예
        // 망가진다.
        dock->setWidget( content, ads::CDockWidget::ForceNoScrollArea );
    }
    return dock;
}

void MainWindow::setupDockLayout()
{
    // 설정 플래그는 **정적**이고 CDockManager 를 만들기 전에 넣어야 한다.
    ads::CDockManager::setConfigFlags( ads::CDockManager::DefaultOpaqueConfig
                                       | ads::CDockManager::FocusHighlighting
                                       | ads::CDockManager::DockAreaHasUndockButton );
    // DefaultAutoHideConfig 가 제목줄의 핀 버튼과 창 가장자리 탭 바를 켠다.
    // AutoHideShowOnMouseOver 를 더하면 Visual Studio 처럼 탭에 마우스를 올리는
    // 것만으로 패널이 겹쳐 나온다 (클릭해야 열리는 것이 기본값이다).
    //
    // AutoHideFlags 로 감싸는 것은 군더더기가 아니다. ADS 는 ConfigFlags 에만
    // Q_DECLARE_OPERATORS_FOR_FLAGS 를 걸어 두었고 AutoHideFlags 에는 걸지
    // 않아서(DockManager.h:914), 열거자끼리 `|` 하면 int 가 되어 컴파일이 막힌다.
    ads::CDockManager::setAutoHideConfigFlags(
            ads::CDockManager::AutoHideFlags( ads::CDockManager::DefaultAutoHideConfig )
            | ads::CDockManager::AutoHideShowOnMouseOver );

    // 부모는 m_centralContainer 다. QMainWindow 를 주면 ADS 가 생성자에서
    // setCentralWidget(this) 를 불러 뷰어 도구모음 슬롯을 날려 버린다
    // (DockManager.cpp 의 생성자). 레이아웃에는 슬롯 **아래**로 들어간다.
    dockManager_ = new ads::CDockManager( m_centralContainer );
    // 예전에 .ui 중앙 위젯이 들고 있던 역할. 드롭 대상이 되어야 파일을 편집기
    // 위로 끌어다 놓을 수 있다 (실제 처리는 MainWindow::dropEvent 가 한다 —
    // 드롭을 받지 않는 자식 위에서는 이벤트가 부모로 올라간다).
    dockManager_->setAcceptDrops( true );
    if( auto* containerLayout = qobject_cast< QVBoxLayout* >( m_centralContainer->layout() ) )
        containerLayout->addWidget( dockManager_, 1 );

    // ── 중앙: 편집기 | 프리뷰 ──
    // 도킹으로 옮기지 않는다. QWebEngineView 는 자기 합성 표면을 갖고 있어
    // 떼어내 별도 창으로 띄우면 페이지가 다시 붙는 동안 화면이 비고, 탭 목록
    // (Ctrl+Tab)이 어느 창에 떠야 하는지가 애매해진다. setCentralWidget() 은
    // 닫기·이동·떼내기·핀 기능을 스스로 꺼 준다.
    dockEditor_ = makeDock( "dock.editor", QString(), Ui.frmContents );
    // CDockWidget 은 기본으로 60x40 을 minimumSizeHint 로 돌려준다(리사이즈를
    // 막지 않으려고). 그러면 frmEditor 240 / frmWebPreview 120 하한이 중앙
    // 영역까지 올라오지 못해서, 창을 좁히면 splitter_2 가 자기 최소 폭 아래로
    // 눌려 편집기와 프리뷰가 잘린다.
    //
    // ...FromContentMinimumSize 가 아니라 ...FromContent 다. 앞의 것은 내용
    // 위젯의 minimumSize() 를 보는데 frmContents 에는 그것을 지정한 곳이 없어
    // (0,0) 이라 아무 일도 하지 않는다. 뒤의 것이 minimumSizeHint() 를 보고,
    // 그 값이 splitter_2 를 거쳐 두 프레임의 하한에서 올라온다.
    dockEditor_->setMinimumSizeHintMode( ads::CDockWidget::MinimumSizeHintFromContent );
    ads::CDockAreaWidget* centralArea = dockManager_->setCentralWidget( dockEditor_ );

    // ── 좌측 ──
    dockExplorer_ = makeDock( "dock.explorer", tr( "탐색기" ), Ui.pnlFolderTree );
    ads::CDockAreaWidget* leftArea =
            dockManager_->addDockWidget( ads::LeftDockWidgetArea, dockExplorer_ );

    // 요약 둘은 탐색기 **아래**에 붙는다 (예전 splFolderWithOutlineOnSide 의 배치).
    dockOutlineDocument_ = makeDock( "dock.outline.document", tr( "활성 문서" ),
                                     Ui.pnlOutlineDocument );
    ads::CDockAreaWidget* outlineArea =
            dockManager_->addDockWidget( ads::BottomDockWidgetArea, dockOutlineDocument_, leftArea );

    dockOutlineProject_ = makeDock( "dock.outline.project", tr( "프로젝트" ),
                                    Ui.pnlOutlineProject );
    dockManager_->addDockWidgetTabToArea( dockOutlineProject_, outlineArea );
    // 예전 tabLeftSideOutline 의 currentIndex 가 1(프로젝트)이었다. 이것은
    // **문서를 모르는 동안의** 기본값이다 — 여기는 생성자라 controller_ 도
    // 열린 파일도 없다. 소속이 정해지면 applyDefaultOutlineTab() 이 다시 정한다.
    dockOutlineProject_->setAsCurrentTab();

    // ── 하단 ──
    dockDiagnostics_ = makeDock( "dock.diagnostics", tr( "진단" ), Ui.pnlDiagnostics );
    dockManager_->addDockWidget( ads::BottomDockWidgetArea, dockDiagnostics_, centralArea );

    dockLog_ = makeDock( "dock.log", tr( "로그" ), Ui.pnlLog );
    if( ads::CDockAreaWidget* bottomArea = dockDiagnostics_->dockAreaWidget() )
        dockManager_->addDockWidgetTabToArea( dockLog_, bottomArea );
    dockDiagnostics_->setAsCurrentTab();

    applyDockStylesheetOverrides();

    // .ui 의 centralwidget 은 이제 비었다 — frmContents 와 pnl* 는 위에서
    // setWidget() 이 도크로 데려갔다. 남겨 두면 안 된다: 빈 위젯이 레이아웃에
    // 붙어 있으면 창의 절반을 먹고, 부모로 남아 있으면 Ui.pnl* 의 소멸 순서가
    // 도크와 엇갈린다.
    delete m_uiCentralShell;
    m_uiCentralShell = nullptr;
}

void MainWindow::applyDefaultDockSizes()
{
    if( dockManager_ == nullptr || dockLayoutFromSession_ )
        return;   // 세션이 정한 배치를 덮지 않는다

    // 창이 실제 크기를 갖기 **전에** 하면 안 된다. QSplitter::setSizes() 는 그
    // 시점의 스플리터 길이가 0 이면 준 값을 전부 0 으로 누르고, 이후 리사이즈에서도
    // 되살리지 않는다. 실측으로 물렸다 — 생성자에서 주었을 때 하단 진단/로그
    // 영역이 0 높이로 열렸다. (ensureVisiblePreviewSplit 이 같은 이유로 같은
    // 자리에서 돈다.)
    //
    // 절대값이 아니라 비율로 준다. 창 크기는 사용자마다 다르고, 세션이 생기면
    // 그 뒤로는 dockLayout 이 이 값을 대신한다.
    const auto ratio = []( QSplitter* splitter, int first, int second ) {
        if( splitter == nullptr || splitter->count() != 2 )
            return;
        const QList< int > sizes = splitter->sizes();
        const int          total = sizes.at( 0 ) + sizes.at( 1 );
        if( total <= 0 )
            return;   // 아직 레이아웃이 없다
        const int head = total * first / ( first + second );
        splitter->setSizes( { head, total - head } );
    };
    const auto areaSplitter = []( ads::CDockWidget* dock ) -> QSplitter* {
        ads::CDockAreaWidget* area = dock != nullptr ? dock->dockAreaWidget() : nullptr;
        return area != nullptr ? area->parentSplitter() : nullptr;
    };

    // 좌측 | 본문. 루트 스플리터를 직접 얻을 방법이 없다(rootSplitter() 는
    // protected) — 좌측 영역에서 도크 매니저가 부모인 스플리터까지 올라간다.
    QSplitter* root = areaSplitter( dockExplorer_ );
    while( root != nullptr && root->parentWidget() != dockManager_ )
        root = qobject_cast< QSplitter* >( root->parentWidget() );
    ratio( root, 1, 4 );

    // 탐색기 | 요약 (좌측 안의 세로 분할)
    ratio( areaSplitter( dockOutlineDocument_ ), 1, 1 );
    // 편집기 | 하단
    ratio( areaSplitter( dockDiagnostics_ ), 4, 1 );
}

void MainWindow::applyDefaultOutlineTab( const bool standalone )
{
    if( dockOutlineDocument_ == nullptr || dockOutlineProject_ == nullptr )
        return;

    // 둘이 한 영역에 탭으로 겹쳐 있을 때만 손댄다. 사용자가 하나를 떼어 냈거나
    // 가장자리에 핀으로 접었거나 위아래로 쪼개 두었으면 ADS 가 저마다 다른
    // 영역을 주고, 그때는 바꿀 탭이 애초에 없다. 영역 포인터는 캐시하지 않는다
    // — 비면 ADS 가 지우고, restoreState() 는 모든 영역을 새로 만든다.
    ads::CDockAreaWidget* area = dockOutlineDocument_->dockAreaWidget();
    if( area == nullptr || area != dockOutlineProject_->dockAreaWidget() )
        return;

    // 스스로 옮기는 방향은 하나다. 되돌리는 것은 **우리가 옮겨 둔** 경우뿐이라,
    // 사용자가 직접 고른 탭은 문서가 바뀌어도 그대로 남는다.
    ads::CDockWidget* target = nullptr;
    if( standalone )
        target = dockOutlineDocument_;
    else if( outlineTabAutoSwitched_ )
        target = dockOutlineProject_;

    // isClosed() 면 setAsCurrentTab() 이 스스로 아무 일도 하지 않지만, 그러면
    // 아래 기록이 화면과 어긋난다. 사용자가 닫아 둔 패널은 되살리지 않는다.
    if( target == nullptr || target->isClosed() || target->isCurrentTab() )
        return;

    // 물러나는 패널은 레이아웃에서 떨어져 숨는다(ADS 의 탭 전환은 내용 위젯을
    // setParent(nullptr) 로 뽑아낸다). 그 안에 포커스가 있으면 사용자가 치던
    // 글자가 갈 곳을 잃는다 — 요약 필터 칸이 실제 사례다. 편집기는 중앙 도크라
    // 여기 걸리지 않으므로 본문을 치는 중에는 규칙이 그대로 동작한다.
    if( ads::CDockWidget* current = area->currentDockWidget();
        current != nullptr && current->isAncestorOf( QApplication::focusWidget() ) )
        return;

    target->setAsCurrentTab();
    // 실제로 앞에 나왔을 때만 기록한다. 되돌릴 대상을 우리 조작으로 한정하려면
    // 이 기록이 화면과 어긋나서는 안 된다.
    outlineTabAutoSwitched_ = dockOutlineDocument_->isCurrentTab();
}

void MainWindow::applyDockStylesheetOverrides()
{
    if( dockManager_ == nullptr )
        return;

    // ADS 의 기본 스타일시트는 비활성 탭의 글자색을 `palette(light)` 로 준다
    // (stylesheets/*_dark.css 의 `ads--CDockWidgetTab QLabel`). Light 는 버튼색을
    // **밝은 쪽으로** 민 음영이라 라이트 테마에서는 맞지만, Qlementine 의 다크
    // 팔레트에서는 창 배경과 거의 같은 값이 되어 글자가 사라진다. 실제로 요약
    // 패널의 "활성 문서 / 프로젝트" 중 선택되지 않은 탭이 안 보였다.
    //
    // 그래서 그 규칙 하나만 테마 색으로 덮는다. 비활성은 흐리게, 활성은 또렷하게
    // — Visual Studio 도 그렇게 구분한다.
    const QColor  fg  = ThemeManager::instance().foregroundColor();
    const QString dim = QStringLiteral( "rgba(%1, %2, %3, 165)" )
                                .arg( fg.red() ).arg( fg.green() ).arg( fg.blue() );

    // ADS 가 만든 스타일시트에 **덧붙인다**. 통째로 갈아치우면 핀/닫기 버튼
    // 아이콘(qproperty-icon)까지 날아간다.
    //
    // 덧붙이기 전에 지난번에 붙인 것을 잘라낸다. ADS 가 스타일시트를 다시 읽는
    // 것은 밝기가 뒤집힐 때뿐인데, themeChanged 는 색만 바꿀 때도 나온다
    // (설정에서 색을 손볼 때마다). 잘라내지 않으면 그때마다 같은 규칙이 한 벌씩
    // 쌓여 스타일시트가 끝없이 자란다.
    static const QString kMarker = QStringLiteral( "\n/* mrst-dock-overrides */" );
    QString              sheet   = dockManager_->styleSheet();
    const int            at      = sheet.indexOf( kMarker );
    if( at >= 0 )
        sheet.truncate( at );

    dockManager_->setStyleSheet(
            sheet + kMarker
            + QStringLiteral( "\nads--CDockWidgetTab QLabel { color: %1; }"
                              "\nads--CDockWidgetTab[activeTab=\"true\"] QLabel { color: %2; }"
                              "\nads--CAutoHideTab { color: %2; }" )
                      .arg( dim, fg.name() ) );
}

void MainWindow::retranslateDockTitles()
{
    // setWindowTitle() 하나가 탭 글자, 가장자리 사이드 탭 글자, 그리고 보기 >
    // 패널의 액션 글자를 함께 갱신한다 (CDockWidget 이 WindowTitleChange 를
    // 받아 세 곳에 옮긴다). 저장/복원 키는 objectName 이라 여기서 바뀌지 않는다.
    const auto title = []( ads::CDockWidget* dock, const QString& text ) {
        if( dock != nullptr )
            dock->setWindowTitle( text );
    };
    title( dockExplorer_,        tr( "탐색기" ) );
    title( dockOutlineDocument_, tr( "활성 문서" ) );
    title( dockOutlineProject_,  tr( "프로젝트" ) );
    title( dockDiagnostics_,     tr( "진단" ) );
    title( dockLog_,             tr( "로그" ) );
    title( dockSearch_,          tr( "검색" ) );
}

void MainWindow::restoreDockLayout( const QString& base64 )
{
    if( dockManager_ == nullptr || base64.isEmpty() )
        return;   // 이 버전보다 먼저 만들어진 세션이다. 기본 배치로 시작한다.

    const QByteArray state = QByteArray::fromBase64( base64.toLatin1() );
    if( state.isEmpty() )
        return;

    // 실패해도 그냥 넘어간다. restoreState() 는 못 읽으면 손대지 않고 false 를
    // 돌려주므로 기본 배치가 남는다 — 배치 하나 때문에 열린 탭을 잃는 것보다
    // 낫다. 실패하는 경우: 저장 당시에 없던 도크가 생겼거나, 그 반대.
    if( dockManager_->restoreState( state ) )
        dockLayoutFromSession_ = true;
    else
        appendLog( tr( "저장된 패널 배치를 읽을 수 없어 기본 배치로 엽니다." ) );
}

void MainWindow::hideAllDockPanels()
{
    if( dockManager_ == nullptr )
        return;

    // dockWidgetsMap() 으로 훑는다. 멤버 포인터를 하나씩 적으면 도크를 새로
    // 더할 때마다 여기를 함께 고쳐야 하고, 잊으면 전체 화면에 패널 하나가
    // 남는다 — 그 버그는 새 패널을 만든 커밋에서는 보이지 않는다.
    const QMap< QString, ads::CDockWidget* > docks = dockManager_->dockWidgetsMap();
    for( ads::CDockWidget* dock : docks )
    {
        if( dock == nullptr || dock == dockEditor_ )
            continue;
        dock->toggleView( false );
    }
}

// ═══════════════════════════════════════════════════════════
// 뷰어별 도구모음 교체
// ═══════════════════════════════════════════════════════════
void MainWindow::updateViewerToolBar()
{
    // 프리뷰는 모든 탭이 공용 WebEngine 하나를 쓴다. 도구모음을 만들 수 없는
    // 로딩 중 탭에서도 이전 탭의 확대 비율이 남지 않도록 먼저 반영한다.
    applyPreviewZoomForCurrentView();

    // 레이아웃 무효화를 이 함수 한 번에 **한 번**으로 모은다.
    //
    // refreshViewerToolBarSlot() 은 호스트와 **부모까지** layout()->invalidate()
    // + activate() 를 돌린다. 예전에는 이 함수 한 번에 그것이 여러 번 불렸다 —
    // 도구모음 파괴에서 둘, 조기 반환에서 하나, 끝에서 하나. 그 각각이
    // splitter_2 를 거쳐 프리뷰 위젯의 geometry 재계산을 유발한다.
    //
    // 스코프 가드로 두면 어느 경로로 반환해도 정확히 한 번 돈다. 그래서 아래
    // 파괴 호출들은 refresh 를 넘기지 않는다(destroyViewerToolBar 의 인자).
    const auto refreshOnce = qScopeGuard( [this] {
        refreshViewerToolBarSlot( m_viewerToolBarHost );
    } );

    // 이전 뷰어 도구모음 즉시 제거 및 삭제
    if( m_viewerAuxToolBar )
        destroyViewerToolBar( m_viewerToolBarHost, m_viewerAuxToolBar, false );
    if( m_viewerToolBar )
        destroyViewerToolBar( m_viewerToolBarHost, m_viewerToolBar, false );

    purgeStaleViewerToolBars( m_viewerToolBarHost );

    auto* view = currentView();
    if( !view )
        return;

    if( view->isLoading() )
    {
        if( m_viewerToolBarHost )
            m_viewerToolBarHost->setVisible( false );
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
        addPreviewZoomControl( m_viewerToolBar, view );

        connect( m_viewerToolBar, &QObject::destroyed, this, [this] {
            m_viewerToolBar = nullptr;
        } );

        if( m_viewerToolBarLayout )
            m_viewerToolBarLayout->addWidget( m_viewerToolBar );

        purgeStaleViewerToolBars( m_viewerToolBarHost, m_viewerToolBar );
        // 프리뷰 전체 화면에서는 도구모음 자리를 되살리지 않는다. 이 함수는 탭이
        // 바뀔 때마다 도는데(Ctrl+Tab 도 그렇다), 그러면 감춰 둔 도구모음이
        // 전체 화면 위로 매번 다시 튀어 오른다.
        if( m_viewerToolBarHost )
            m_viewerToolBarHost->setVisible( !previewFullScreen_.active );

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
    }
}

int MainWindow::previewZoomPercentForView( const QBaseView* view ) const
{
    if( !isPreviewDocumentView( view ) )
        return mrst::kDefaultPreviewZoomPercent;

    const QVariant viewValue = view->property( kPreviewZoomProperty );
    if( viewValue.isValid() )
    {
        const int percent = viewValue.toInt();
        if( percent >= mrst::kMinimumPreviewZoomPercent
            && percent <= mrst::kMaximumPreviewZoomPercent )
            return percent;
    }

    const QString path = normalizeFilePath( view->currentFilePath() );
    return previewZoomPercentByPath_.value( path, mrst::kDefaultPreviewZoomPercent );
}

void MainWindow::applyPreviewZoomForCurrentView()
{
    QBaseView* view = currentView();
    const int percent = previewZoomPercentForView( view );
    if( isPreviewDocumentView( view ) )
        view->setProperty( kPreviewZoomProperty, percent );

    if( previewInitialised_ && Ui.webEngineView != nullptr )
        Ui.webEngineView->setZoomFactor( static_cast< qreal >( percent ) / 100.0 );
}

void MainWindow::setPreviewZoomPercentForView( QBaseView* view, const int percent )
{
    if( !isPreviewDocumentView( view )
        || percent < mrst::kMinimumPreviewZoomPercent
        || percent > mrst::kMaximumPreviewZoomPercent )
        return;

    view->setProperty( kPreviewZoomProperty, percent );
    const QString path = normalizeFilePath( view->currentFilePath() );
    if( !path.isEmpty() )
    {
        if( percent == mrst::kDefaultPreviewZoomPercent )
            previewZoomPercentByPath_.remove( path );
        else
            previewZoomPercentByPath_.insert( path, percent );
    }

    if( currentView() == view && previewInitialised_ && Ui.webEngineView != nullptr )
        Ui.webEngineView->setZoomFactor( static_cast< qreal >( percent ) / 100.0 );

    // 콤보 선택은 한 번에 끝나는 조작이라 디바운스가 필요 없다. 여기서 바로
    // workspace.json 을 갱신해야 탭을 닫은 뒤 다시 열어도 파일별 값이 남는다.
    saveWorkspaceSessionNow();
}

void MainWindow::addPreviewZoomControl( QToolBar* toolBar, QBaseView* view )
{
    if( toolBar == nullptr || !isPreviewDocumentView( view ) )
        return;

    toolBar->addSeparator();

    // 레이블과 콤보를 하나의 도구모음 항목으로 묶는다. 따로 추가하면 공간이
    // 좁을 때 레이블만 확장 메뉴로 밀려 콤보의 의미가 보이지 않을 수 있다.
    auto* controls = new QWidget( toolBar );
    controls->setObjectName( QStringLiteral( "previewZoomControl" ) );
    auto* controlsLayout = new QHBoxLayout( controls );
    controlsLayout->setContentsMargins( 0, 0, 0, 0 );
    controlsLayout->setSpacing( 6 );

    auto* label = new QLabel( tr( "프리뷰 확대 비율" ) + QLatin1Char( ':' ), controls );
    label->setObjectName( QStringLiteral( "previewZoomLabel" ) );
    // QToolBar 의 자식 QLabel 은 Qlementine 스타일에서 ButtonText 역할을 상속할
    // 수 있다. 그 색이 도구모음 배경색과 같아지는 테마에서도 보이도록 실제 전경
    // 역할을 WindowText 로 되돌리고 모든 상태 그룹에 명시적인 글자색을 지정한다.
    label->setForegroundRole( QPalette::WindowText );
    const QColor foreground = ThemeManager::instance().foregroundColor();
    QPalette labelPalette = label->palette();
    labelPalette.setColor( QPalette::All, QPalette::WindowText, foreground );
    labelPalette.setColor( QPalette::All, QPalette::ButtonText, foreground );
    labelPalette.setColor( QPalette::All, QPalette::Text, foreground );
    label->setPalette( labelPalette );

    auto* combo = new QComboBox( controls );
    combo->setObjectName( QStringLiteral( "previewZoomCombo" ) );
    combo->setAccessibleName( tr( "프리뷰 확대 비율" ) );
    combo->setToolTip( tr( "프리뷰 확대 비율" ) );
    combo->setMaximumWidth( 90 );
    for( const int option : { 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200 } )
        combo->addItem( QStringLiteral( "%1%" ).arg( option ), option );

    const int percent = previewZoomPercentForView( view );
    int index = combo->findData( percent );
    if( index < 0 )
    {
        // 수동으로 편집했거나 미래 버전이 남긴 유효한 값을 잃지 않는다.
        index = 0;
        while( index < combo->count() && combo->itemData( index ).toInt() < percent )
            ++index;
        combo->insertItem( index, QStringLiteral( "%1%" ).arg( percent ), percent );
    }
    combo->setCurrentIndex( index );
    connect( combo, QOverload<int>::of( &QComboBox::currentIndexChanged ), combo,
             [this, view, combo]( const int selectedIndex ) {
                 setPreviewZoomPercentForView( view, combo->itemData( selectedIndex ).toInt() );
             } );
    label->setBuddy( combo );
    controlsLayout->addWidget( label );
    controlsLayout->addWidget( combo );
    toolBar->addWidget( controls );
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
            // 이미 열려 있어도 "방금 본 문서" 다. 목록의 맨 앞으로 올린다.
            addRecentFile( normalizedPath );
            m_tabWidget->setCurrentIndex( i );
            // 트리뷰·진단 표·개요에서 문서를 불러낸 경우 포커스는 아직 그 패널에
            // 있다. 문서를 앞에 냈으면 키보드도 문서에 있어야 한다. 미루는 이유는
            // addViewTab() 쪽과 같다.
            QTimer::singleShot( 0, this, &MainWindow::focusActiveEditor );
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

    // 여기까지 왔으면 열기가 시작됐다(비동기 경로는 아직 읽는 중이다). 실패
    // 경로는 모두 위에서 돌아갔으므로, 못 여는 파일이 목록에 쌓이지 않는다.
    addRecentFile( normalizedPath );
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

    // 패널 아이콘은 그려서 만든 것이라 팔레트를 따라가지 않는다. 테마가 바뀌면
    // 다시 그려야 어두운 테마에 검은 아이콘이 남는 일이 없다.
    applyExplorerIcons();

    for( int i = 0; i < m_tabWidget->count(); ++i )
    {
        if( auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( i ) ) )
        {
            applyThemeToView( view );
            updateTabDecoration( view );
        }
    }

    updateViewerToolBar();

    // 도크 스타일시트 덧붙임은 **미뤄서** 다시 넣는다. ADS 는 팔레트가 바뀌면
    // 자기 eventFilter 에서 스타일시트를 통째로 다시 읽어(setStyleSheet) 우리가
    // 덧붙인 규칙을 지운다. 그 처리가 지금 이 호출과 같은 이벤트 전달 안에서
    // 일어나므로, 여기서 바로 덧붙이면 곧이어 덮인다.
    QTimer::singleShot( 0, this, &MainWindow::applyDockStylesheetOverrides );

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

void MainWindow::applyConfiguredFonts()
{
    auto& themeManager = ThemeManager::instance();
    themeManager.applyToApplication();

    if( Ui.treLeftSideFolterTree != nullptr )
        Ui.treLeftSideFolterTree->setFont(
            ThemeManager::configuredFont( ThemeManager::FontRole::Explorer ) );

    const QFont outlineFont = ThemeManager::configuredFont( ThemeManager::FontRole::Outline );
    if( Ui.treOutlineDocument != nullptr )
        Ui.treOutlineDocument->setFont( outlineFont );
    if( Ui.treOutlineProject != nullptr )
        Ui.treOutlineProject->setFont( outlineFont );

    const QFont diagnosticsFont =
        ThemeManager::configuredFont( ThemeManager::FontRole::DiagnosticsAndLog );
    if( Ui.tblDiagnostics != nullptr )
        Ui.tblDiagnostics->setFont( diagnosticsFont );
    if( Ui.logView != nullptr )
        Ui.logView->setFont( diagnosticsFont );

    // 검색 페이지는 별도 글꼴 범위가 생기기 전까지 전역 UI 글꼴을 명시적으로
    // 따른다. 런타임에 만든 페이지라 QApplication 전파 시점을 가정하지 않는다.
    if( searchTabPage_ != nullptr )
        searchTabPage_->setFont(
            ThemeManager::configuredFont( ThemeManager::FontRole::UserInterface ) );
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

// ═══════════════════════════════════════════════════════════
// 외부 파일 편집 인식
// ═══════════════════════════════════════════════════════════
void MainWindow::refreshExternalWatchSet()
{
    if( externalWatcher_ == nullptr || m_tabWidget == nullptr )
        return;

    QStringList paths;
    for( int i = 0; i < m_tabWidget->count(); ++i )
    {
        auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( i ) );
        if( view == nullptr )
            continue;

        const QString path = view->currentFilePath();
        if( !path.trimmed().isEmpty() )
            paths.append( path );
    }

    externalWatcher_->setWatchedFiles( paths );
}

QTextView* MainWindow::textViewForPath( const QString& filePath ) const
{
    if( m_tabWidget == nullptr )
        return nullptr;

    const QString wanted = normalizeFilePath( filePath );
    if( wanted.isEmpty() )
        return nullptr;

    for( int i = 0; i < m_tabWidget->count(); ++i )
    {
        auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( i ) );
        if( view == nullptr )
            continue;
        if( normalizeFilePath( view->currentFilePath() ) == wanted )
            return textViewOf( view );
    }

    return nullptr;
}

void MainWindow::connectViewWatchSignals( QBaseView* view )
{
    auto* textView = qobject_cast< QTextView* >( view );
    if( textView == nullptr || externalWatcher_ == nullptr )
        return;

    connect( textView, &QTextView::sigFileWriteStarted, this, [this]( const QString& path ) {
        externalWatcher_->beginSelfWrite( path );
    } );
    connect( textView, &QTextView::sigFileSaved, this, [this]( const QString& path ) {
        // "다른 이름으로 저장" 이면 감시 대상 경로가 바뀐다. 목록을 먼저 맞춰야
        // endSelfWrite() 가 새 경로의 항목을 찾아 기준을 다시 잡을 수 있다.
        refreshExternalWatchSet();
        externalWatcher_->endSelfWrite( path );
    } );
    connect( textView, &QBaseView::sigFileOpened, this, [this]( const QString& ) {
        refreshExternalWatchSet();
    } );
    // 다시 읽기는 비동기다. 시작할 때 알리면 아직 옛 본문을 보고 있는 사람에게
    // 끝났다고 말하는 셈이 된다.
    //
    // 감시자에게 markSynchronized() 를 부르지 않는다. 그것은 "지금 디스크" 를
    // 기준으로 삼는데, 우리가 읽은 시점 뒤에 또 바뀌었다면 그 변경을 영원히
    // 놓친다. 발견 시점의 기준을 그대로 두면 그 경우 한 번 더 알려 준다.
    connect( textView, &QTextView::sigFileReloadedFromDisk, this, [this]( const QString& path ) {
        showTransientStatus( tr( "밖에서 바뀐 내용으로 다시 불러왔습니다: %1" )
                                     .arg( QFileInfo( path ).fileName() ),
                             kExternalChangeStatusMs );
    } );
}

void MainWindow::onExternalFileChanged( const QString& filePath )
{
    if( externalWatcher_ == nullptr || m_shuttingDown )
        return;

    QTextView* view = textViewForPath( filePath );
    if( view == nullptr )
    {
        mrst::traceP( "watch.noView", filePath );
        refreshExternalWatchSet();          // 이미 닫힌 탭이다
        return;
    }

    if( externalWatcher_->action() == mrst::ExternalChangeWatcher::Action::Ignore )
        return;

    // 저장하지 않은 편집이 있으면 "자동 불러오기" 라도 반드시 묻는다. 여기서
    // 조용히 덮으면 사용자가 방금 친 것이 되돌릴 수 없이 사라진다 — 그 설정을
    // 고른 사람이 승낙한 것은 그 위험이 아니다.
    const bool mayReloadSilently =
        externalWatcher_->action() == mrst::ExternalChangeWatcher::Action::Reload
        && !view->isModified();

    if( !mayReloadSilently )
    {
        mrst::traceP( "watch.ask", filePath );
        queueExternalChangePrompt( filePath );
        return;
    }

    // 알림은 읽기가 끝난 뒤 sigFileReloadedFromDisk 를 받아서 낸다.
    mrst::traceP( "watch.reload", filePath );
    reloadViewFromDisk( view, filePath );
}

void MainWindow::reloadViewFromDisk( QTextView* view, const QString& filePath )
{
    if( view == nullptr || view->reloadFromDisk() )
        return;

    // 그 뷰가 이미 읽거나 쓰는 중이면 지금은 바꿀 수 없다. 감시자는 이 변경을
    // 알리면서 기준을 갱신했으므로, 여기서 놓치면 **영원히** 반영되지 않는다.
    // 로드는 반드시 끝나므로 이 되풀이는 스스로 멎는다.
    if( !view->isLoading() )
        return;

    QTimer::singleShot( kExternalReloadRetryMs, this, [this, filePath] {
        reloadViewFromDisk( textViewForPath( filePath ), filePath );
    } );
}

void MainWindow::onExternalFileVanished( const QString& filePath )
{
    if( externalWatcher_ == nullptr || m_shuttingDown )
        return;

    QTextView* view = textViewForPath( filePath );
    if( view == nullptr )
    {
        refreshExternalWatchSet();
        return;
    }

    if( externalWatcher_->action() == mrst::ExternalChangeWatcher::Action::Ignore )
        return;

    // 탭을 닫지 않는다. 이 버퍼가 그 내용의 마지막 사본일 수 있다.
    view->markFileVanished();
    showTransientStatus( tr( "파일이 밖에서 사라졌습니다. 편집 중인 내용은 그대로 두었습니다: %1" )
                                 .arg( QFileInfo( filePath ).fileName() ),
                         kExternalChangeStatusMs );
}

void MainWindow::queueExternalChangePrompt( const QString& filePath )
{
    const QString normalized = normalizeFilePath( filePath );
    if( normalized.isEmpty() )
        return;

    if( !externalPromptQueue_.contains( normalized ) )
        externalPromptQueue_.append( normalized );
    flushExternalChangePrompts();
}

void MainWindow::flushExternalChangePrompts()
{
    // 창이 활성이 아니면 묻지 않는다. 다른 앱에서 일하는 사람 앞에 모달을
    // 들이밀면 그 사람이 치던 글자가 어디로 갔는지 알 수 없게 된다. 창으로
    // 돌아올 때 changeEvent() 가 이 함수를 다시 부른다.
    if( externalPromptActive_ || m_shuttingDown || !isActiveWindow() )
        return;

    externalPromptActive_ = true;
    while( !externalPromptQueue_.isEmpty() && !m_shuttingDown )
    {
        const QString path = externalPromptQueue_.takeFirst();
        QTextView* view = textViewForPath( path );
        if( view == nullptr )
            continue;

        const bool hadUnsavedEdits = view->isModified();

        QMessageBox box( this );
        box.setIcon( hadUnsavedEdits ? QMessageBox::Warning : QMessageBox::Question );
        box.setWindowTitle( tr( "밖에서 바뀐 파일" ) );
        box.setText( tr( "다른 프로그램이 이 파일을 바꿨습니다.\n%1" )
                        .arg( QDir::toNativeSeparators( path ) ) );
        box.setInformativeText( hadUnsavedEdits
            ? tr( "이 탭에는 저장하지 않은 편집이 있습니다. 다시 불러오면 그 편집은 사라집니다." )
            : tr( "디스크의 내용으로 다시 불러올까요?" ) );

        QPushButton* reloadButton = box.addButton( tr( "다시 불러오기" ), QMessageBox::AcceptRole );
        QPushButton* keepButton   = box.addButton( tr( "그대로 두기" ), QMessageBox::RejectRole );
        // 잃을 것이 있는 쪽에서는 기본 단추를 안전한 쪽에 둔다. Enter 를 습관처럼
        // 누르는 손이 저장하지 않은 편집을 지우게 만들지 않는다.
        box.setDefaultButton( hadUnsavedEdits ? keepButton : reloadButton );
        box.exec();

        if( box.clickedButton() != reloadButton )
            continue;

        // 대화상자가 이벤트 루프를 돌린 동안 탭이 닫혔을 수 있다.
        reloadViewFromDisk( textViewForPath( path ), path );
    }
    externalPromptActive_ = false;
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
    // 도구모음을 만들 때 쓸 부모. 뷰가 아니라 슬롯이어야 하는 이유는
    // QBaseView::setToolBarHost() 주석에 있다 (Qlementine 콤보박스 무한 재귀).
    view->setToolBarHost( m_viewerToolBarHost );

    const int idx = m_tabWidget->addTab( view, view->title() );
    m_tabWidget->setCurrentIndex( idx );
    updateTabDecoration( view );
    // 문서를 열었으면 키보드도 문서에 있어야 한다. 여기가 새 탭이 생기는 유일한
    // 자리라 탐색기·진단·개요·드롭·최근 파일이 모두 이 한 줄을 지난다.
    //
    // **이벤트 루프로 미룬다.** 여기서 곧바로 setFocus 하면 트리가 포커스를
    // 되가져간다 — 트리의 더블클릭 처리가 아직 끝나지 않았고, 새 탭 페이지도
    // 아직 보이지 않아 Qt 가 포커스를 실제로 옮기지 않는다. 기동 경로가
    // 같은 이유로 같은 방식을 쓴다(advanceStartupPhase).
    //
    // 세션 복원도 이 자리를 지나가지만, 기동 끝에서 한 번 더 돌아 결과는
    // 달라지지 않는다.
    QTimer::singleShot( 0, this, &MainWindow::focusActiveEditor );

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
    refreshExternalWatchSet();
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

    // 탭이 사라졌으므로 그 파일의 감시도 접는다. 종료 경로에서는 곧 창이
    // 내려가니 굳이 목록을 다시 만들지 않는다.
    if( !m_shuttingDown )
        refreshExternalWatchSet();
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

    connectViewWatchSignals( view );

    if( auto* textView = qobject_cast< QTextView* >( view ) )
    {
        // 세 시그널 모두 **한 번의 조작에 여러 번** 온다. 줄넘김 토글 하나에
        // statusChanged 가 3~4번 오는데(setWordWrapMode 가 직접 하나,
        // SCN_UPDATEUI 가 커서·선택 경로로 둘~셋) 그때마다 상태바를 처음부터 다시
        // 만들 이유가 없다. 이벤트 루프 한 회전에 한 번으로 접는다.
        const auto schedule = [this, textView] {
            if( currentView() == textView )
                scheduleStatusBarRefresh();
        };
        connect( textView, &QTextView::statusChanged, this, schedule );
        connect( textView, &QTextView::encodingChanged, this, schedule );
        connect( textView, &QTextView::languageChanged, this, schedule );
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
    const QString startDir = controller_ ? controller_->workspaceRoot() : QString{};
    const QString folder = QFileDialog::getExistingDirectory( this, tr( "워크스페이스 폴더 열기" ), startDir );
    if( !folder.isEmpty() )
        setWorkspace( folder );
}

void MainWindow::onWorkspaceClose()
{
    if( workspaceRoot_.isEmpty() )
        return;

    if( setWorkspace( {} ) )
        showTransientStatus( tr( "워크스페이스를 닫았습니다." ), 2500 );
}

bool MainWindow::closeWorkspaceTabs()
{
    if( m_tabWidget == nullptr )
        return true;

    QVector< QBaseView* > views;
    QVector< QWidget* >   otherWidgets;
    QVector< QTextView* > discardedTextViews;
    views.reserve( m_tabWidget->count() );
    otherWidgets.reserve( m_tabWidget->count() );

    for( int index = 0; index < m_tabWidget->count(); ++index )
    {
        QWidget* widget = m_tabWidget->widget( index );
        auto*    view = qobject_cast< QBaseView* >( widget );
        if( view == nullptr )
        {
            otherWidgets.push_back( widget );
            continue;
        }

        // 읽기뿐 아니라 저장도 같은 loading 상태를 쓴다. 저장 중인 뷰를 파일
        // 열기 취소처럼 teardown 하면 백그라운드 쓰기의 완료 통지를 잃으므로,
        // 작업이 끝난 뒤 사용자가 다시 전환하도록 둔다.
        if( view->isLoading() )
        {
            showTransientStatus(
                tr( "파일 작업이 진행 중입니다. 완료된 뒤 워크스페이스를 다시 닫거나 전환해 주세요." ),
                3500 );
            return false;
        }

        if( view->isModified() )
        {
            const QString label = view->currentFilePath().isEmpty()
                                      ? view->title()
                                      : QDir::toNativeSeparators( view->currentFilePath() );
            const auto answer = QMessageBox::question(
                this, tr( "저장 확인" ),
                tr( "워크스페이스를 닫으면 열린 탭도 닫힙니다.\n"
                    "변경사항을 저장하시겠습니까?\n%1" ).arg( label ),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel );
            if( answer == QMessageBox::Cancel )
                return false;

            if( answer == QMessageBox::Yes )
            {
                if( !saveView( view, false ) )
                    return false;
                if( view->isLoading() )
                {
                    showTransientStatus(
                        tr( "저장이 진행 중입니다. 완료된 뒤 워크스페이스를 다시 닫거나 전환해 주세요." ),
                        3500 );
                    return false;
                }
            }
            else if( auto* textView = textViewOf( view ) )
            {
                // 모든 확인이 끝나기 전에는 백업을 지우지 않는다. 뒤쪽 문서에서
                // 취소한 경우 현재 워크스페이스가 그대로 남기 때문이다.
                discardedTextViews.push_back( textView );
            }
        }

        views.push_back( view );
    }

    for( QTextView* view : std::as_const( discardedTextViews ) )
        view->abandonHotExitBackup();

    for( QBaseView* view : std::as_const( views ) )
        teardownView( view );

    for( QWidget* widget : std::as_const( otherWidgets ) )
    {
        if( widget == nullptr )
            continue;
        const int index = m_tabWidget->indexOf( widget );
        if( index >= 0 )
        {
            const QSignalBlocker blocker( m_tabWidget );
            m_tabWidget->removeTab( index );
        }
        widget->deleteLater();
    }

    if( controller_ != nullptr )
        controller_->setActiveDocument( nullptr );
    tabMruOrder_.clear();
    if( !tabSwitcher_.isNull() )
        tabSwitcher_->hide();
    refreshCurrentViewUi();
    return true;
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
        // 탭 하나를 닫는 것은 hot exit 가 맡지 않는다. 종료와 달리 사용자가 그
        // 문서를 지금 치우겠다고 지목한 것이라, 저장할지 물어야 뜻을 확인할 수
        // 있다. 여기서도 묻기를 건너뛰면 "아니요" 를 고를 자리가 없어져, 버리려던
        // 변경이 다음 실행에 되살아난다.
        if( view->isModified() )
        {
            auto btn = QMessageBox::question( this, tr( "저장 확인" ),
                tr( "변경사항이 있습니다. 저장하시겠습니까?" ),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel );
            if( btn == QMessageBox::Cancel ) return;
            if( btn == QMessageBox::No )
            {
                // 버리겠다고 했다. 그 사이 주기 타이머가 써 둔 hot exit 백업을
                // 지우고 다시 못 쓰게 잠근다. 남겨 두면 다음 실행이 이 문서를
                // 되살려, 사용자가 버린 변경이 돌아온다.
                if( auto* textView = qobject_cast< QTextView* >( view ) )
                    textView->abandonHotExitBackup();
            }
            else if( btn == QMessageBox::Yes )
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
                    showTransientStatus( tr( "저장이 진행 중입니다. 저장 완료 후 다시 탭을 닫아 주세요." ), 3000 );
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

    // 탭을 지울 때 currentChanged 를 막아 두므로(removeViewTabWithoutSignals, 그리고
    // 위 else 의 QSignalBlocker) "새 탭이 앞에 왔을 때" 처리가 저절로 돌지 않는다.
    // 직접 부른다 — 포커스뿐 아니라 활성 문서(프리뷰·LSP)와 Ctrl+Tab 순서도
    // 여기서 갱신된다. 이것이 없으면 지워진 위젯이 포커스를 쥔 채 부모에서 떨어져
    // 나가므로 Qt 가 포커스 체인의 다음 위젯 — 탐색기 트리 — 로 포커스를 넘긴다.
    if( m_tabWidget->count() > 0 )
    {
        onTabChanged( m_tabWidget->currentIndex() );
        focusActiveEditor();
    }
    else
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
        // global thread pool의 대기 작업은 위 clear()에서 시작도 못 한 채
        // 제거될 수 있다. 그 작업이 PathIndex 스캔이었다면 scanning 상태만
        // 남으므로, 종료 취소 뒤 현재 워크스페이스 인덱스를 새 세대로 다시 건다.
        if( mrst::PathIndex* pathIndex = controller_->pathIndex() )
        {
            pathIndex->clear();
            if( !workspaceRoot_.isEmpty() )
                pathIndex->ensure( workspaceRoot_ );
        }
        controller_->setActiveDocument( textViewOf( currentView() ) );
    };

    if( m_tabWidget )
    {
        for( int i = 0; i < m_tabWidget->count(); ++i )
        {
            QWidget* widget = m_tabWidget->widget( i );
            if( auto* view = qobject_cast< QBaseView* >( widget ) )
            {
                // hot exit 가 켜져 있으면 묻지 않고 스냅샷으로 남긴다. 다음
                // 실행이 restoreHotExitSnapshots() 와 세션 복원으로 되살린다.
                //
                // 조건을 flush 의 **결과**로 따진다. 스냅샷을 쓰지 못했는데
                // (폴더 권한, 디스크 가득, 아직 읽는 중) 묻기까지 건너뛰면
                // 사용자는 아무 표시도 없이 변경을 잃는다. 그때는 평소의 저장
                // 확인으로 물러선다.
                bool keptByHotExit = false;
                if( view->isModified() && canCloseWithTextHotExit( view ) )
                {
                    if( auto* textView = qobject_cast< QTextView* >( view ) )
                        keptByHotExit = textView->flushHotExitBackup();
                }

                if( view->isModified() && !keptByHotExit )
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

    // 예약된 INI 쓰기를 내보낸다. 이 지점이어야 하는 이유가 둘 있다 — 저장 확인을
    // 모두 통과해 종료가 확정된 뒤이고(취소로 되돌아가는 경로에서는 예약을 그대로
    // 남겨 둬야 한다), 아직 이벤트 루프가 살아 있어 파일 쓰기가 정상 경로로 돈다.
    mrst::SettingsWriter::instance().flush();

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
    noteTabActivated( currentView() );

    if( controller_ )
        controller_->setActiveDocument( textViewOf( currentView() ) );

    // Markdown 에는 reST/Sphinx 진단과 이전 문서의 실행 로그가 해당되지 않는다.
    // 저장소는 보존해 reST 탭으로 돌아갔을 때 다시 표시하고, 현재 패널만 비운다.
    if( isMarkdownView( currentView() ) )
    {
        documentPanelsHiddenForMarkdown_ = true;
        if( Ui.tblDiagnostics != nullptr )
            Ui.tblDiagnostics->setRowCount( 0 );
        if( Ui.logView != nullptr )
            Ui.logView->clear();
    }
    else if( documentPanelsHiddenForMarkdown_ )
    {
        documentPanelsHiddenForMarkdown_ = false;
        scheduleDiagnosticsTableRefresh();
    }

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

void MainWindow::togglePreviewFullScreen()
{
    setPreviewFullScreen( !previewFullScreen_.active );
}

void MainWindow::setPreviewFullScreen( const bool enabled )
{
    if( previewFullScreen_.active == enabled )
        return;

    // 진입과 복귀를 각각 잰다. 처음 쟀을 때 진입 34.5 ms / 복귀 65.3 ms 였고,
    // 아래 갱신 억제와 도구모음 재생성 시점 변경으로 16 / 24 ms 가 되었다.
    const mrst::PhaseSpan span( enabled ? "fullscreen.enter" : "fullscreen.exit" );

    // 기동 직후라면 프리뷰가 아직 붙지 않았다. 그 상태로 들어가면 빈 화면만
    // 남고 되돌아올 단서(메뉴)도 사라진다. 파일 열기와 같은 처리를 한다 —
    // 사용자 조작이 기동 단계를 앞당긴다.
    if( enabled )
    {
        initialisePreview();
        if( Ui.webEngineView == nullptr )
            return;
    }

    previewFullScreen_.active = enabled;
    if( previewFullScreenAction_ != nullptr )
        previewFullScreenAction_->setChecked( enabled );
    if( previewExitFullScreenAction_ != nullptr )
        previewExitFullScreenAction_->setEnabled( enabled );

    // 진입·복귀 모두 위젯을 여러 개 숨기고 보이며 창 상태를 바꾼다. 억제 구간이
    // 없으면 그 단계마다 splitter_2 를 거쳐 프리뷰 위젯의 geometry 가 다시
    // 계산되고, 이 저장소의 Breathe 페이지는 하나가 24 MB 라 그 재계산 하나가
    // 눈에 보이는 정지가 된다. 복귀는 아래 singleShot 블록 끝에서 되살린다.
    setUpdatesEnabled( false );

    // 전환 동안 프리뷰 위젯을 hide() 하지는 않는다. 리사이즈 전파를 한 번으로
    // 모으려고 해 봤지만 fs.dock.restore 도 정체도 줄지 않았고(진입은 16.8 →
    // 23 ms 로 늘었다) 전체 화면이 빈 화면으로 남았다.
    //
    // 남은 정체는 Qt 의 리사이즈 전파가 아니라 24 MB 페이지를 다루는 Chromium
    // 쪽에 있다 — 같은 시나리오를 2 KB 페이지로 돌리면 F11 정체 합계가 6.4초에서
    // 1.9초로 줄고 1초 이상 정체는 사라진다.

    if( enabled )
    {
        // 진입은 이 함수 안에서 끝나므로 여기서 되살린다.
        const auto restoreUpdates = qScopeGuard( [this] { setUpdatesEnabled( true ); } );

        previewFullScreen_.windowStates = windowState();
        // 세션에 남길 창 크기도 여기서 뜬다. 전체 화면 도중의 saveGeometry() 에는
        // 전체 화면 플래그가 담겨, 그대로 저장하면 다음 실행이 메뉴도 편집기도
        // 없는 창으로 열린다(currentWindowGeometry 참고).
        previewFullScreen_.windowGeometry = saveGeometry();
        previewFullScreen_.previewSplitSizes = Ui.splitter_2->sizes();
        // 좌측·하단은 도크 배치 전체를 한 덩어리로 기억한다. "보였는가" 만
        // 남기면 핀 고정해 둔 패널이 핀이 풀린 채로, 하단에서 로그를 보고
        // 있었으면 진단 탭으로 돌아온다.
        //
        // 직전 복귀의 배치 복원이 아직 돌지 않았다면 스냅샷을 덮지 않는다.
        //
        // 복원은 singleShot(0) 안에 있다. 그것이 뜨기 전에 F11 이 다시 들어오면
        // 도크는 아직 **전부 닫힌** 상태이므로, 지금 saveState() 를 찍으면 그
        // "아무것도 없는 배치" 를 정상 배치로 기억하게 된다. 그러면 다음 복귀에서
        // 패널이 통째로 사라지고, 사용자는 되돌릴 방법이 없다.
        if( !fullScreenRestorePending_ )
            previewFullScreen_.dockState = dockManager_->saveState();
        previewFullScreen_.editorVisible = Ui.frmEditor->isVisible();
        previewFullScreen_.menuBarVisible = menuBar()->isVisible();
        previewFullScreen_.statusBarVisible = statusBar()->isVisible();

        // 알림 바(missingDepBar_ / updateBar_)는 건드리지 않는다. 전체 화면
        // 도중에 새로 뜰 수 있어서, 감췄다가 되돌리면 그 사이에 생긴 알림을
        // 조용히 지우게 된다. 둘 다 한 줄짜리이고 사용자가 닫을 수 있다.

        // 메뉴 바를 감추기 **전에** 그 안의 단축키를 창으로 빌려 온다
        // (borrowedActions 옆 주석 참고). 이 줄이 없으면 전체 화면에서
        // Ctrl+S 도 Ctrl+W 도 F11 도 듣지 않는다 — 되돌아올 수단이 사라진다.
        previewFullScreen_.borrowedActions.clear();
        const QList< QAction* > menuActions = menuBar()->findChildren< QAction* >();
        for( QAction* action : menuActions )
        {
            if( action == nullptr || action->shortcut().isEmpty() )
                continue;
            previewFullScreen_.borrowedActions.push_back( action );
            addAction( action );
        }

        // 중앙(편집기|프리뷰)만 남기고 모든 도크를 닫는다. 가장자리에 핀 고정해
        // 둔 패널도 이걸로 함께 걷힌다 — 마지막 탭이 사라지면 사이드 탭 바가
        // 스스로 숨는다.
        hideAllDockPanels();
        Ui.frmEditor->hide();
        menuBar()->hide();
        statusBar()->hide();
        if( m_viewerToolBarHost != nullptr )
            m_viewerToolBarHost->hide();

        showFullScreen();
        // 프리뷰에 포커스를 준다. PageDown 으로 곧바로 읽어 내려갈 수 있어야
        // 전체 화면이 제 일을 한다. F11 은 ApplicationShortcut 이라 Chromium 이
        // 포커스를 쥐고 있어도 계속 듣는다.
        Ui.webEngineView->setFocus();
        return;
    }

    for( const QPointer< QAction >& action : previewFullScreen_.borrowedActions )
    {
        if( !action.isNull() )
            removeAction( action );
    }
    previewFullScreen_.borrowedActions.clear();

    Ui.frmEditor->setVisible( previewFullScreen_.editorVisible );
    menuBar()->setVisible( previewFullScreen_.menuBarVisible );
    statusBar()->setVisible( previewFullScreen_.statusBarVisible );

    if( ( previewFullScreen_.windowStates & Qt::WindowMaximized ) != 0 )
        showMaximized();
    else
        showNormal();

    // 배치 복원은 창이 원래 크기로 돌아온 **뒤에** 해야 한다. QSplitter 는
    // setSizes() 로 준 값의 합과 실제 폭이 다르면 남는 폭을 스트레치 비율로
    // 나눠 주므로, 전체 화면 폭에서 넣은 값은 그 자리에서 뭉개진다.
    fullScreenRestorePending_ = true;
    QTimer::singleShot( 0, this, [this] {
        // 어떤 경로로 빠져나가도 이 둘은 되돌린다. 갱신을 껐는데 켜지 않으면
        // 창이 통째로 멈춘 것처럼 보이고, 대기 표시를 지우지 않으면 다음 진입이
        // 배치 스냅샷을 영영 갱신하지 못한다.
        const auto finish = qScopeGuard( [this] {
            fullScreenRestorePending_ = false;
            setUpdatesEnabled( true );
        } );

        if( previewFullScreen_.active )
            return;   // 그 사이 다시 들어갔다

        // 이 람다는 setPreviewFullScreen() 이 반환한 뒤에 돈다. 위쪽
        // "fullscreen.exit" span 이 여기를 감싸지 못하므로 따로 잡는다.
        const mrst::PhaseSpan deferredSpan( "fullscreen.exit.deferred" );

        // 도크 배치가 먼저다. restoreState() 가 편집기|프리뷰 스플리터를 담은
        // 중앙 도크를 재배치하므로, 순서를 뒤집으면 아래 setSizes() 가 그
        // 재배치에 덮인다.
        if( !previewFullScreen_.dockState.isEmpty() )
        {
            // ADS 는 도크 매니저 전체를 hide()/show() 하고 XML 을 두 번 파싱한다.
            // 그 매니저 안에 프리뷰가 있어 Chromium 표면이 함께 내려갔다 올라온다.
            const mrst::PhaseSpan restoreSpan( "fs.dock.restore" );
            dockManager_->restoreState( previewFullScreen_.dockState );
        }

        if( Ui.splitter_2 != nullptr
            && previewFullScreen_.previewSplitSizes.size() == Ui.splitter_2->count() )
        {
            Ui.splitter_2->setSizes( previewFullScreen_.previewSplitSizes );
        }

        // 도구모음은 **여기서** 다시 만든다. 전체 화면 동안 탭이 바뀌었을 수
        // 있으므로(Ctrl+Tab) 다시 계산해야 하는 것은 맞지만, 예전에는 그것을
        // showNormal() 앞에서 했다 — 전체 화면 폭으로 만든 콤보박스 넷과
        // 스핀박스가 곧바로 창 폭으로, 그다음 restoreState() 로, 그다음
        // setSizes() 로 세 번 더 재배치되었다. 최종 폭이 정해진 뒤에 한 번 만든다.
        {
            const mrst::PhaseSpan toolbarSpan( "fs.toolbar.rebuild" );
            updateViewerToolBar();
        }

        if( QBaseView* view = currentView() )
            view->setFocus();
    } );
}

void MainWindow::noteTabActivated( QBaseView* view )
{
    // 죽은 항목을 여기서 걷어낸다. 탭이 사라지는 경로가 여럿이라(닫기 버튼,
    // 종료, 뷰 교체) 각 자리에서 지우는 대신 목록을 만질 때마다 정리한다.
    tabMruOrder_.removeIf( []( const QPointer< QBaseView >& entry ) { return entry.isNull(); } );
    if( view == nullptr )
        return;

    tabMruOrder_.removeAll( QPointer< QBaseView >( view ) );
    tabMruOrder_.prepend( view );
}

void MainWindow::showTabSwitcher( const bool forward )
{
    if( m_tabWidget == nullptr || m_tabWidget->count() < 2 )
        return;

    // 최근 사용 순서로 세운다. 목록에 없는 탭(막 열려 아직 활성화된 적이 없는
    // 것)은 뒤에 탭 순서로 붙인다 — 빠뜨리면 그 탭으로는 갈 수 없다.
    tabMruOrder_.removeIf( []( const QPointer< QBaseView >& entry ) { return entry.isNull(); } );
    QList< QBaseView* > ordered;
    for( const QPointer< QBaseView >& entry : tabMruOrder_ )
    {
        QBaseView* view = entry.data();
        if( view != nullptr && m_tabWidget->indexOf( view ) >= 0 && !ordered.contains( view ) )
            ordered.push_back( view );
    }
    for( int index = 0; index < m_tabWidget->count(); ++index )
    {
        auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( index ) );
        if( view != nullptr && !ordered.contains( view ) )
            ordered.push_back( view );
    }
    if( ordered.size() < 2 )
        return;

    QList< mrst::TabSwitcherEntry > entries;
    entries.reserve( ordered.size() );
    for( QBaseView* view : ordered )
    {
        const int index = m_tabWidget->indexOf( view );
        mrst::TabSwitcherEntry entry;
        // 탭 바에 보이는 문자열을 그대로 쓴다. 수정 표시(●)까지 같아야 목록과
        // 탭 바가 같은 것을 가리킨다고 읽힌다.
        entry.title = m_tabWidget->tabText( index );
        entry.detail = view->currentFilePath();
        entry.icon = m_tabWidget->tabIcon( index );
        entry.tabIndex = index;
        entries.push_back( entry );
    }

    if( tabSwitcher_.isNull() )
    {
        tabSwitcher_ = new mrst::TabSwitcherPopup( this );
        connect( tabSwitcher_, &mrst::TabSwitcherPopup::tabChosen, this, [this]( const int tabIndex ) {
            // 탭을 옮기면 onTabChanged 가 컨트롤러에 활성 문서를 알리고, 그것이
            // 프리뷰 빌드까지 끌고 간다. 전체 화면에서도 같은 경로다.
            if( m_tabWidget != nullptr && tabIndex >= 0 && tabIndex < m_tabWidget->count() )
                m_tabWidget->setCurrentIndex( tabIndex );
        } );
    }

    // 앞으로는 1번(= 직전 문서), 뒤로는 마지막(= 가장 오래 안 본 문서)부터
    // 강조한다. 0번은 지금 보고 있는 문서라 첫 강조 자리로는 의미가 없다.
    tabSwitcher_->showEntries( entries, forward ? 1 : entries.size() - 1 );
}

QStringList MainWindow::quickOpenRecentPaths( const QString& root ) const
{
    QStringList relativePaths;
    if( root.isEmpty() )
        return relativePaths;

    const QDir rootDir( QDir::cleanPath( root ) );
    for( const QString& recentPath : m_recentFiles )
    {
        QString relative = QDir::fromNativeSeparators(
            rootDir.relativeFilePath( QDir::cleanPath( recentPath ) ) );
        while( relative.startsWith( QLatin1String( "./" ) ) )
            relative.remove( 0, 2 );

        // 다른 드라이브는 절대 경로로, 다른 상위 폴더는 ../ 로 돌아온다.
        // 빠른 열기는 현재 워크스페이스 밖으로 나가서는 안 된다.
        if( relative.isEmpty() || QDir::isAbsolutePath( relative ) || relative == QLatin1String( ".." )
            || relative.startsWith( QLatin1String( "../" ) ) )
            continue;
        if( !relativePaths.contains( relative, Qt::CaseInsensitive ) )
            relativePaths.push_back( relative );
    }
    return relativePaths;
}

void MainWindow::showQuickOpen()
{
    if( workspaceRoot_.isEmpty() || controller_ == nullptr )
    {
        showTransientStatus( tr( "열린 워크스페이스가 없습니다." ), 3000 );
        return;
    }

    // 경로 정규화/QDir 접근보다 먼저 본다. 연결이 끊긴 매핑 드라이브는 경로를
    // 확인하는 것만으로도 Windows 재연결 대기에 들어갈 수 있다.
    if( mrst::isDisconnectedRemoteDrivePath( workspaceRoot_ ) )
    {
        showTransientStatus( tr( "원격 드라이브가 연결되어 있지 않습니다: %1" )
                                 .arg( QDir::toNativeSeparators( workspaceRoot_ ) ), 4000 );
        return;
    }

    mrst::PathIndex* index = controller_->pathIndex();
    if( index == nullptr )
    {
        showTransientStatus( tr( "파일 인덱스를 사용할 수 없습니다." ), 3000 );
        return;
    }

    if( quickOpenDialog_.isNull() )
    {
        quickOpenDialog_ = new mrst::QuickOpenDialog( this );
        connect( quickOpenDialog_, &mrst::QuickOpenDialog::fileChosen,
                 this, [this]( const QString& path ) { openFile( path ); } );

        connect( index, &mrst::PathIndex::scanStarted, this,
                 [this, index]( const QString& root ) {
                     if( quickOpenDialog_.isNull() || !quickOpenDialog_->isVisible()
                         || root.compare( workspaceRoot_, Qt::CaseInsensitive ) != 0 )
                         return;
                     // 같은 루트 재스캔이면 완성된 이전 snapshot만 유지한다. 새
                     // partial을 섞으면 fast path에는 중복 제거가 없어 같은 파일이
                     // 두 줄 생긴다. ready에서 최종 snapshot으로 바꾼다.
                     if( index->isReadyFor( root ) )
                     {
                         // 같은 루트의 완성 snapshot은 이미 표시 중이다. 재스캔
                         // 시작 시 다시 교체하면 변화가 없어도 전체 랭킹한다.
                         quickOpenDialog_->setIndexingProgress( index->paths().size() );
                     }
                     else
                     {
                         quickOpenDialog_->replacePathIndexChunks(
                             index->partialPathChunks(), index->scannedPathCount() );
                     }
                 } );
        connect( index, &mrst::PathIndex::progress, this,
                 [this, index]( const QString& root, const QStringList& batch,
                         const qsizetype scannedCount ) {
                     if( quickOpenDialog_.isNull() || !quickOpenDialog_->isVisible()
                         || root.compare( workspaceRoot_, Qt::CaseInsensitive ) != 0 )
                         return;
                     if( index->isReadyFor( root ) )
                     {
                         quickOpenDialog_->setIndexingProgress( scannedCount );
                         return;
                     }
                     quickOpenDialog_->appendPathIndexBatch( batch, scannedCount );
                 } );
        connect( index, &mrst::PathIndex::ready, this,
                 [this, index]( const QString& root, qsizetype ) {
                     if( quickOpenDialog_.isNull() || !quickOpenDialog_->isVisible()
                         || root.compare( workspaceRoot_, Qt::CaseInsensitive ) != 0 )
                         return;
                     quickOpenDialog_->finishPathIndexing( index->paths() );
                 } );
    }

    QStringList paths;
    const mrst::PathIndexChunks* partialChunks = nullptr;
    qsizetype scannedPathCount = 0;
    bool indexing = false;
    if( index->isScanningFor( workspaceRoot_ ) )
    {
        // 재스캔에서는 이전 완성 snapshot, 첫 스캔에서만 partial을 보인다.
        if( index->isReadyFor( workspaceRoot_ ) )
            paths = index->paths();
        else
        {
            partialChunks = &index->partialPathChunks();
            scannedPathCount = index->scannedPathCount();
        }
        indexing = true;
    }
    else if( index->isReadyFor( workspaceRoot_ ) )
    {
        paths = index->paths();
    }
    else
    {
        indexing = true;
        index->ensure( workspaceRoot_ );
        if( index->isScanningFor( workspaceRoot_ ) )
        {
            partialChunks = &index->partialPathChunks();
            scannedPathCount = index->scannedPathCount();
        }
    }

    const QAction* quickOpenAction =
        menuBar()->findChild<QAction*>( QStringLiteral( "file.quickOpen" ) );
    const QString shortcutText = quickOpenAction != nullptr
                                     ? quickOpenAction->shortcut().toString(
                                           QKeySequence::NativeText )
                                     : QString{};
    const QStringList recentPaths = quickOpenRecentPaths( workspaceRoot_ );
    if( partialChunks != nullptr )
    {
        quickOpenDialog_->showForPathIndexChunks(
            workspaceRoot_, *partialChunks, indexing, scannedPathCount,
            recentPaths, shortcutText );
    }
    else
    {
        quickOpenDialog_->showForPathIndex( workspaceRoot_, paths, indexing,
                                            recentPaths, shortcutText );
    }
}

QString MainWindow::stampLogLine( const QString& text )
{
    // 시각은 번역하지 않는다 — 언어에 따라 자릿수가 달라지면 칸이 어긋난다.
    // 월은 MM, 분은 mm 이다(Qt 형식 문자열). 연도는 넣지 않는다: 로그는 이번
    // 실행 동안의 것이고, 한 줄이 길어지면 정작 메시지가 밀려난다.
    const QString stamp = QStringLiteral( "[ %1 ] " )
                                  .arg( QDateTime::currentDateTime().toString(
                                          QStringLiteral( "MM-dd HH:mm:ss.zzz" ) ) );

    if( !text.contains( QChar( QChar::LineFeed ) ) )
        return stamp + text;

    // 여러 줄짜리 메시지(빌드 출력, 파이썬 트레이스백)는 한 사건이다. 줄마다
    // 같은 시각을 되풀이하면 읽기 어려우므로 첫 줄에만 찍고 나머지는 같은 칸만큼
    // 밀어 한 덩어리로 보이게 한다.
    const QString indent( stamp.size(), QLatin1Char( ' ' ) );
    QStringList   lines = text.split( QChar( QChar::LineFeed ) );
    for( int i = 1; i < lines.size(); ++i )
        lines[ i ].prepend( indent );
    return stamp + lines.join( QChar( QChar::LineFeed ) );
}

void MainWindow::appendLog( const QString& text )
{
    if( Ui.logView == nullptr )
        return;

    const auto trimmed = text.trimmed();
    if( trimmed.isEmpty() == true )
        return;

    const QString stamped = stampLogLine( trimmed );
    Ui.logView->appendPlainText( stamped );

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
        stream << stamped << Qt::endl;
    }
}

void MainWindow::onSettings()
{
    QSettingsDialog dlg( this );
    connect( &dlg, &QSettingsDialog::settingsApplied, this, [this] {
        // 단축키 즉시 적용
        const auto shortcuts = QSettingsDialog::LoadShortcutsFromSettings();
        QSettingsDialog::ApplyShortcutsToActions( shortcuts, this );
        // 메뉴·도구모음·도킹 탭과 패널별 글꼴을 즉시 적용한다.
        applyConfiguredFonts();
        // 열려있는 뷰어에 변경된 설정 적용
        applySettingsToAllViews();
        // 개요 트리 깊이는 컨트롤러에 다시 묻지 않고 캐시한 개요로 다시 그린다.
        reloadOutlineDepth();
        // 스캐너 제외 목록 / 최대 Esbonio 프로세스 수 등도 즉시 반영한다.
        if( controller_ != nullptr )
            controller_->reloadSettings();
        if( externalWatcher_ != nullptr )
            externalWatcher_->reloadSettings();
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

void MainWindow::scheduleDiagnosticsTableRefresh()
{
    if( diagnosticsTableRefreshPending_ )
        return;

    diagnosticsTableRefreshPending_ = true;
    QTimer::singleShot( 0, this, [this] {
        diagnosticsTableRefreshPending_ = false;
        refreshDiagnosticsTable();
    } );
}

void MainWindow::scheduleStatusBarRefresh()
{
    if( statusBarRefreshPending_ )
        return;

    statusBarRefreshPending_ = true;
    // singleShot(0) 은 지금 처리 중인 이벤트가 끝난 뒤 같은 스레드에서 돈다.
    // 즉 한 조작이 낸 시그널 여러 개가 하나로 접힌다.
    QTimer::singleShot( 0, this, [this] {
        statusBarRefreshPending_ = false;
        updateStatusBar();
    } );
}

void MainWindow::startStallWatchdog()
{
    if( !mrst::phaseTraceEnabled() )
        return;   // 배포 빌드에서는 타이머 자체를 만들지 않는다

    constexpr int kIntervalMs = 50;
    /// 이 이상 늦으면 남긴다. 타이머 정확도(윈도우 기본 틱)와 정상적인 페인트
    /// 한 장을 넘기는 값이어야 한다 — 그보다 낮으면 트레이스가 잡음으로 덮인다.
    constexpr double kReportMs = 60.0;

    stallClock_.start();
    stallWatchdog_ = new QTimer( this );
    stallWatchdog_->setTimerType( Qt::PreciseTimer );
    stallWatchdog_->setInterval( kIntervalMs );
    connect( stallWatchdog_, &QTimer::timeout, this, [this] {
        const qint64 nowNs = stallClock_.nsecsElapsed();
        if( stallLastTickNs_ != 0 )
        {
            const double gapMs = static_cast< double >( nowNs - stallLastTickNs_ ) / 1e6;
            if( gapMs >= kReportMs )
                mrst::traceP( "ui.stall", QStringLiteral( "%1ms" ).arg( gapMs, 0, 'f', 1 ) );
        }
        stallLastTickNs_ = nowNs;
    } );
    stallWatchdog_->start();
}

void MainWindow::updateStatusBar()
{
    // 캐럿을 움직일 때마다 도는 함수다. characterCount() 가 문서를 통째로
    // 복사하는지(1.1 의 근거) 여기 값으로 가른다 — 문서 크기에 비례하면 그렇다.
    const mrst::PhaseSpan span( "status.update" );

    // 지난 알림을 치운다. 문서가 바뀌었으면 그 알림은 더 이상 지금 화면에
    // 대한 이야기가 아니다.
    clearTransientStatus();

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
        showTransientStatus(
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
    if( statusMessageLabel_ == nullptr || statusProgressBar_ == nullptr )
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
            showTransientStatus( message, 2000 );
        else if( m_activeViewLoads.isEmpty() )
            updateStatusBar();
    }

    updateSaveActionState();
}

void MainWindow::refreshLoadingIndicator()
{
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
        statusTasks_.remove( StatusTaskId::FileLoad );
        refreshStatusProgress();
        return;
    }

    const ViewLoadingState state = m_activeViewLoads.value( displayView );
    QString effectiveMessage = state.message.isEmpty()
        ? tr( "파일 여는 중..." )
        : state.message;
    if( !displayView->title().isEmpty() )
        effectiveMessage = tr( "%1 — %2" ).arg( displayView->title(), effectiveMessage );

    StatusTask task;
    task.cancellable = displayView->canCancelLoading();
    task.cancelTip   = tr( "\"%1\" 파일 열기를 취소합니다." ).arg( displayView->title() );

    if( state.maximum > 0 )
    {
        const int boundedValue = qBound( 0, state.value, state.maximum );
        const int percent = qRound( ( static_cast< double >( boundedValue ) * 100.0 ) / state.maximum );
        task.message  = tr( "%1 (%2%)" ).arg( effectiveMessage ).arg( percent );
        task.permille = qRound( ( static_cast< double >( boundedValue ) * 1000.0 ) / state.maximum );
    }
    else
    {
        task.message  = effectiveMessage;
        task.permille = -1;
    }

    statusTasks_.insert( StatusTaskId::FileLoad, task );
    refreshStatusProgress();
}

void MainWindow::refreshStatusProgress()
{
    if( statusMessageLabel_ == nullptr || statusProgressBar_ == nullptr )
        return;

    const auto resetCancel = [this]( const bool visible, const QString& tip ) {
        if( statusCancelButton_ == nullptr )
            return;
        statusCancelButton_->setVisible( visible );
        statusCancelButton_->setToolTip( tip );
    };

    if( statusTasks_.isEmpty() )
    {
        statusMessageLabel_->setText( transientStatus_ );
        statusMessageLabel_->setVisible( !transientStatus_.isEmpty() );
        statusProgressBar_->setVisible( false );
        statusProgressBar_->reset();
        statusProgressBar_->setFormat( QStringLiteral( "%p%" ) );
        resetCancel( false, {} );
        return;
    }

    // QMap 은 키로 정렬하므로 첫 항목이 곧 우선순위가 가장 높은 작업이다
    // (StatusTaskId 의 선언 순서).
    const StatusTask front = *statusTasks_.constBegin();

    // 큰 문서를 열면 파일 읽기와 프리뷰가 겹친다. 앞선 것만 보여 주면 남은 일이
    // 몇 개인지 알 수 없어 "다 됐는데 왜 아직" 처럼 보인다.
    //: 상태표시줄에 동시에 도는 작업이 둘 이상일 때의 틀. %1 은 작업 개수,
    //: %2 는 그중 지금 보여 주는(첫) 작업의 안내문이다. 보이는 것은 언제나
    //: 첫 작업이라 앞의 1 은 상수다 — 예: "(1/2) 파일 여는 중...".
    statusMessageLabel_->setText(
            statusTasks_.size() > 1
                    ? tr( "(1/%1) %2" ).arg( statusTasks_.size() ).arg( front.message )
                    : front.message );
    statusMessageLabel_->setVisible( true );

    // 전체 진행도는 값을 아는 작업들의 평균이다. 하나도 모를 때만 왕복 막대로
    // 넘어간다 — 아는 값이 하나라도 있으면 그것이 왕복 막대보다 쓸모 있다.
    int    known = 0;
    qint64 sum   = 0;
    for( const StatusTask& task : std::as_const( statusTasks_ ) )
    {
        if( task.permille < 0 )
            continue;
        ++known;
        sum += task.permille;
    }

    statusProgressBar_->setVisible( true );
    if( known > 0 )
    {
        // 범위를 천분율로 두고 %p% 를 쓰면 Qt 가 백분율로 환산해 보여 준다.
        statusProgressBar_->setRange( 0, 1000 );
        statusProgressBar_->setValue( static_cast< int >( sum / known ) );
        statusProgressBar_->setFormat( QStringLiteral( "%p%" ) );
    }
    else
    {
        statusProgressBar_->setRange( 0, 0 );
        statusProgressBar_->setFormat( tr( "작업 중" ) );
    }

    resetCancel( front.cancellable, front.cancelTip );
}

void MainWindow::showTransientStatus( const QString& text, const int msec )
{
    transientStatus_ = text.trimmed();

    if( transientStatusTimer_ != nullptr )
    {
        transientStatusTimer_->stop();
        if( msec > 0 && !transientStatus_.isEmpty() )
            transientStatusTimer_->start( msec );
    }

    refreshStatusProgress();
}

void MainWindow::clearTransientStatus()
{
    showTransientStatus( {} );
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
        showTransientStatus( tr( "드롭한 항목 %n개를 처리했습니다.", nullptr, openedRequests ), 2500 );
}

void MainWindow::openDroppedDirectory( const QString& dirPath )
{
    // 예전에는 폴더 안의 첫 이미지를 먼저 찾아 열었다. 이미지 뷰어가 없는 지금은
    // 그 파일이 텍스트로 열려 깨져 보이기만 하므로 그냥 직계 파일들을 연다.
    QDir dir( dirPath );
    const QFileInfoList entries = dir.entryInfoList( QDir::Files, QDir::Name | QDir::IgnoreCase );
    if( entries.isEmpty() )
    {
        showTransientStatus( tr( "디렉토리에 열 수 있는 직계 파일이 없습니다: %1" ).arg( QDir::toNativeSeparators( dirPath ) ), 3500 );
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
// 최근 파일 / 워크스페이스
//
// 두 목록을 **한 메뉴**에 위아래로 놓는다. 파일 메뉴에 형제 항목을 하나 더
// 늘리는 대신 이렇게 하는 이유는, 둘이 답하는 질문이 "지난번에 뭘 하고
// 있었나" 하나로 같기 때문이다. 섹션 제목이 어느 쪽인지 알려 준다.
// ═══════════════════════════════════════════════════════════
void MainWindow::addRecentFile( const QString& filePath )
{
    const QStringList updated = mrst::prependRecentEntry( m_recentFiles, filePath );
    if( updated == m_recentFiles )
        return;   // 이미 맨 앞이다. 메뉴를 다시 그릴 이유도 없다.

    m_recentFiles = updated;
    // 세션 복원은 문서 수만큼 이 함수를 부른다. AppSettings 는 소멸할 때마다
    // ini 를 통째로 다시 쓰므로(solSettingsWriter.hpp 참고) 기록기를 거친다.
    mrst::SettingsWriter::instance().setValue( QStringLiteral( "recentFiles" ), m_recentFiles );
    updateRecentFilesMenu();
}

void MainWindow::addRecentWorkspace( const QString& folderPath )
{
    const QStringList updated = mrst::prependRecentEntry( m_recentWorkspaces, folderPath );
    if( updated == m_recentWorkspaces )
        return;

    m_recentWorkspaces = updated;
    mrst::SettingsWriter::instance().setValue( QStringLiteral( "recentWorkspaces" ),
                                              m_recentWorkspaces );
    updateRecentFilesMenu();
}

void MainWindow::dropRecentFile( const QString& filePath )
{
    const QStringList pruned = mrst::removeRecentEntry( m_recentFiles, filePath );
    if( pruned == m_recentFiles )
        return;

    m_recentFiles = pruned;
    mrst::SettingsWriter::instance().setValue( QStringLiteral( "recentFiles" ), m_recentFiles );
    updateRecentFilesMenu();
}

void MainWindow::dropRecentWorkspace( const QString& folderPath )
{
    const QStringList pruned = mrst::removeRecentEntry( m_recentWorkspaces, folderPath );
    if( pruned == m_recentWorkspaces )
        return;

    m_recentWorkspaces = pruned;
    mrst::SettingsWriter::instance().setValue( QStringLiteral( "recentWorkspaces" ),
                                              m_recentWorkspaces );
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    if( !m_recentMenu ) return;
    m_recentMenu->clear();

    // 항목 글자는 이름만 쓰고 전체 경로는 툴팁에 둔다. 워크스페이스마다
    // `index.rst` 가 있으므로 이름만으로는 어느 것인지 알 수 없는데, 그렇다고
    // 전체 경로를 글자로 쓰면 메뉴가 화면 밖까지 넓어진다.
    const auto addEntry = [this]( const QString& path, const QString& text,
                                  void ( MainWindow::*activate )( const QString& ) ) {
        QAction* action = m_recentMenu->addAction( text, this, [this, path, activate] {
            // **여는 일을 미룬다.** 무엇을 열든 그 항목은 목록의 맨 앞으로
            // 올라오고, 그러면 이 함수가 다시 돌면서 `QMenu::clear()` 가 **지금
            // 시그널을 내고 있는 이 QAction 을 즉시 delete** 한다(메뉴가 소유자다).
            // 미루면 그 삭제가 QAction::activate() 의 스택이 다 빠져나간 뒤에 온다.
            QTimer::singleShot( 0, this, [this, path, activate] {
                ( this->*activate )( path );
            } );
        } );
        action->setToolTip( QDir::toNativeSeparators( path ) );
        action->setStatusTip( QDir::toNativeSeparators( path ) );
    };

    // **`addSection()` 을 쓰지 않는다.** 이 앱의 스타일(qlementine)에서는 섹션의
    // 글자가 그려지지 않아, 두 묶음이 제목도 구분선도 없이 붙어 나온다. 실측으로
    // 확인했다. 어느 쪽이 파일이고 어느 쪽이 워크스페이스인지 알 수 없는 메뉴가
    // 되므로, 스타일에 기대지 않는 비활성 항목으로 제목을 만든다.
    const auto addHeader = [this]( const QString& text ) {
        m_recentMenu->addAction( text )->setEnabled( false );
    };

    addHeader( tr( "파일" ) );
    if( m_recentFiles.isEmpty() )
        m_recentMenu->addAction( tr( "(없음)" ) )->setEnabled( false );
    for( const QString& path : m_recentFiles )
        addEntry( path, QFileInfo( path ).fileName(), &MainWindow::openRecentFile );

    m_recentMenu->addSeparator();
    addHeader( tr( "워크스페이스" ) );
    if( m_recentWorkspaces.isEmpty() )
        m_recentMenu->addAction( tr( "(없음)" ) )->setEnabled( false );
    for( const QString& path : m_recentWorkspaces )
    {
        // 폴더 이름만으로는 `docs` 가 여럿이라 구분되지 않는다. 한 단계 위까지
        // 붙여 "부모/폴더" 로 보인다 (경로 전체는 툴팁에 있다).
        const QDir dir( path );
        const QString parent = QFileInfo( dir.absolutePath() ).dir().dirName();
        const QString label = parent.isEmpty()
            ? QDir::toNativeSeparators( path )
            : QStringLiteral( "%1 / %2" ).arg( parent, dir.dirName() );
        addEntry( path, label, &MainWindow::openRecentWorkspace );
    }

    // QMenu 는 기본적으로 툴팁을 띄우지 않는다. 위에서 넣은 경로가 보이려면
    // 이 한 줄이 필요하다.
    m_recentMenu->setToolTipsVisible( true );
}

void MainWindow::openRecentFile( const QString& filePath )
{
    if( mrst::isDisconnectedRemoteDrivePath( filePath ) )
    {
        // 연결 끊김은 파일 삭제와 다르다. 최근 목록은 남겨 두어 재연결 뒤 다시
        // 열 수 있게 하고, QFileInfo 로 들어가 Windows 재연결을 기다리지 않는다.
        showTransientStatus( tr( "원격 드라이브가 연결되어 있지 않습니다: %1" )
                                 .arg( QDir::toNativeSeparators( filePath ) ), 4000 );
        return;
    }

    if( !QFileInfo::exists( filePath ) )
    {
        // 지워졌거나 옮겨졌다. 실패를 알리고 목록에서 뺀다 — 그대로 두면 같은
        // 실패를 반복할 때까지 자리를 차지한다.
        showTransientStatus( tr( "파일을 찾을 수 없습니다: %1" )
                                 .arg( QDir::toNativeSeparators( filePath ) ), 4000 );
        dropRecentFile( filePath );
        return;
    }

    openFile( filePath );
}

void MainWindow::openRecentWorkspace( const QString& folderPath )
{
    if( mrst::isDisconnectedRemoteDrivePath( folderPath ) )
    {
        showTransientStatus( tr( "원격 드라이브가 연결되어 있지 않습니다: %1" )
                                 .arg( QDir::toNativeSeparators( folderPath ) ), 4000 );
        return;
    }

    if( !QFileInfo( folderPath ).isDir() )
    {
        showTransientStatus( tr( "워크스페이스 폴더를 찾을 수 없습니다: %1" )
                                 .arg( QDir::toNativeSeparators( folderPath ) ), 4000 );
        dropRecentWorkspace( folderPath );
        return;
    }

    // 지금 워크스페이스와 같으면 아무것도 하지 않는다. setWorkspace() 를 그냥
    // 다시 부르면 트리 루트를 새로 잡고 전체 스캔이 한 번 더 돈다.
    if( !workspaceRoot_.isEmpty()
        && QFileInfo( folderPath ).absoluteFilePath().compare(
               workspaceRoot_, Qt::CaseInsensitive ) == 0 )
        return;

    setWorkspace( folderPath );
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
        // 트리를 훑던 일감을 먼저 버린다. 남겨 두면 모델을 떼어낸 뒤에도 타이머가
        // 한 번 더 깨어나 죽은 인덱스를 만진다.
        stopExplorerFilterWalk();
        if( Ui.treLeftSideFolterTree != nullptr )
            Ui.treLeftSideFolterTree->setModel( nullptr );
        // 프록시도 놓아야 한다. 뷰에서만 떼면 프록시가 여전히 원본을 붙들고
        // 있어 모델의 수집 스레드가 계속 돈다.
        if( explorerProxy_ != nullptr )
            explorerProxy_->setSourceModel( nullptr );
        // 빈 루트로 되돌리면 Windows 의 모든 드라이브 열거를 다시 시작한다.
        // 모델은 창과 함께 곧 파괴되므로 여기서는 소스 연결만 끊으면 충분하다.
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

void MainWindow::resetWorkspaceUi()
{
    if( !quickOpenDialog_.isNull() )
        quickOpenDialog_->close();

    stopExplorerFilterWalk();
    explorerExpandedBeforeFilter_.clear();
    externalPromptQueue_.clear();

    if( Ui.edtExplorerFilter != nullptr )
        Ui.edtExplorerFilter->clear();
    if( explorerFilterDebounce_ != nullptr )
        explorerFilterDebounce_->stop();
    if( explorerProxy_ != nullptr )
        explorerProxy_->setFilterText( {} );
    if( Ui.treLeftSideFolterTree != nullptr )
    {
        Ui.treLeftSideFolterTree->collapseAll();
        Ui.treLeftSideFolterTree->clearSelection();
    }

    documentPanelsHiddenForMarkdown_ = false;
    if( Ui.tblDiagnostics != nullptr )
        Ui.tblDiagnostics->setRowCount( 0 );
    if( Ui.logView != nullptr )
        Ui.logView->clear();

    outlineDocumentSymbols_.clear();
    outlineProjectDocuments_.clear();
    outlineProjectTruncated_ = 0;
    outlineTabAutoSwitched_ = false;
    if( Ui.edtOutlineDocumentFilter != nullptr )
        Ui.edtOutlineDocumentFilter->clear();
    if( Ui.edtOutlineProjectFilter != nullptr )
        Ui.edtOutlineProjectFilter->clear();

    if( searchQueryEdit_ != nullptr )
        searchQueryEdit_->clear();
    if( searchReplaceEdit_ != nullptr )
        searchReplaceEdit_->clear();
    if( searchResultTree_ != nullptr )
        searchResultTree_->clear();
    pendingReplacePaths_.clear();
    if( searchApplyButton_ != nullptr )
        searchApplyButton_->setEnabled( false );
    if( searchStatusLabel_ != nullptr )
        searchStatusLabel_->setText( tr( "워크스페이스와 찾을 내용을 지정하세요." ) );

    missingDepPending_.clear();
    missingDepDismissed_.clear();
    if( missingDepBar_ != nullptr )
        missingDepBar_->setVisible( false );

}

bool MainWindow::setWorkspace( const QString& Folder )
{
    if( !Folder.isEmpty() && mrst::isDisconnectedRemoteDrivePath( Folder ) )
    {
        showTransientStatus( tr( "원격 드라이브가 연결되어 있지 않습니다: %1" )
                                 .arg( QDir::toNativeSeparators( Folder ) ), 4000 );
        return false;
    }

    const QString workspaceRoot = Folder.isEmpty()
                                      ? QString{}
                                      : QFileInfo( Folder ).absoluteFilePath();
    if( workspaceRoot.compare( workspaceRoot_, Qt::CaseInsensitive ) == 0 )
        return true;

    // 열린 탭과 배치는 워크스페이스 세션에 속한다. 먼저 세션을 남긴 뒤 탭을
    // 정리해야 B 워크스페이스에서 A 문서가 계속 프리뷰/LSP 대상이 되지 않는다.
    if( !workspaceRoot_.isEmpty() )
    {
        saveWorkspaceSessionNow();
        if( !closeWorkspaceTabs() )
            return false;
    }

    resetWorkspaceUi();
    workspaceRoot_ = workspaceRoot;
    previewZoomPercentByPath_.clear();
    if( !workspaceRoot.isEmpty() )
    {
        const mrst::WorkspaceSession session = mrst::loadWorkspaceSession( workspaceRoot );
        for( auto it = session.previewZoomPercentByPath.cbegin();
             it != session.previewZoomPercentByPath.cend(); ++it )
        {
            const QString normalizedPath = normalizeFilePath( it.key() );
            if( !normalizedPath.isEmpty() )
                previewZoomPercentByPath_.insert( normalizedPath, it.value() );
        }
    }
    mrst::SettingsWriter::instance().setValue( QStringLiteral( "workspace/lastRoot" ), workspaceRoot );
    if( m_closeWorkspaceAction != nullptr )
        m_closeWorkspaceAction->setEnabled( !workspaceRoot.isEmpty() );

    // 세션 복원으로 들어온 경로도 그대로 넣는다. 맨 앞으로 올라올 뿐이고,
    // 그것이 "지난번에 보고 있던 워크스페이스" 라는 사실과도 맞다.
    if( !workspaceRoot.isEmpty() )
        addRecentWorkspace( workspaceRoot );

    // 지난 워크스페이스를 좇던 일감을 버린다. 그 인덱스들은 이제 트리 밖이다.
    stopExplorerFilterWalk();
    QModelIndex sourceRoot;
    if( treLeftFolderTreeModel_ != nullptr )
    {
        treLeftFolderTreeModel_->setRootPath( workspaceRoot );
        if( !workspaceRoot.isEmpty() )
            sourceRoot = treLeftFolderTreeModel_->index( workspaceRoot );
    }

    if( explorerProxy_ != nullptr )
    {
        // 프록시가 조상 검사를 멈출 자리다. 이것이 없으면 워크스페이스 폴더
        // 이름이 우연히 필터와 맞는 순간 필터가 통째로 무력해진다.
        explorerProxy_->setRootSourceIndex( sourceRoot );
        if( Ui.treLeftSideFolterTree != nullptr )
            Ui.treLeftSideFolterTree->setRootIndex( explorerProxy_->mapFromSource( sourceRoot ) );
    }
    else if( Ui.treLeftSideFolterTree != nullptr )
    {
        Ui.treLeftSideFolterTree->setRootIndex( sourceRoot );
    }

    if( treLeftFolderTreeModel_ != nullptr && Ui.treLeftSideFolterTree != nullptr )
    {
        for( int column = 1; column < treLeftFolderTreeModel_->columnCount(); ++column )
            Ui.treLeftSideFolterTree->hideColumn( column );
    }

    // 필터를 걸어 둔 채로 워크스페이스를 옮겼으면 새 뿌리에서 다시 훑는다.
    if( !workspaceRoot.isEmpty() && explorerProxy_ != nullptr && explorerProxy_->isFiltering() )
        beginExplorerFilterWalk();

    // 스캔은 컨트롤러가 백그라운드로 수행하고 결과를 로그/시그널로 알려준다.
    if( controller_ )
        controller_->setWorkspaceRoot( workspaceRoot );
    showPreviewStartPage();
    applyPreviewZoomForCurrentView();

    return true;
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
        connect( store, &mrst::DiagnosticsStore::changed, this,
                &MainWindow::scheduleDiagnosticsTableRefresh );
    }
}

// ═══════════════════════════════════════════════════════════
// 탐색기 패널
// ═══════════════════════════════════════════════════════════
namespace {

/// 필터 한 번에 훑을 디렉터리 수의 상한.
///
/// QFileSystemModel 은 게을러서 읽으라고 해야 읽는다. 상한이 없으면 사용자가
/// 드라이브 루트를 워크스페이스로 열었을 때 한 글자에 디스크 전체를 훑는다.
/// 예산이 떨어지면 거기서 멈춘다 — 결과가 모자랄 수는 있어도 앱이 멎지는 않는다.
///
/// 폴더 하나는 예산을 둘 쓴다(읽으라고 넣을 때 한 번, 다 읽히고 그 안을 볼 때
/// 한 번). 그러니 실제로 닿는 폴더 수는 이 값의 절반쯤이다.
constexpr int kExplorerWalkBudget = 4000;

/// 한 차례에 쓸 시간. 이만큼 지나면 남은 일감을 두고 이벤트 루프로 돌아간다.
///
/// **개수가 아니라 시간으로 끊는다.** 폴더 하나를 펼치는 값은 그 안에 몇 개가
/// 들었는지, Debug 인지 Release 인지, 디스크가 무엇인지에 따라 열 배씩 다르다.
/// 개수로 끊으면 그중 한 경우에만 맞고 나머지에서는 너무 길거나(입력이 밀린다)
/// 너무 짧다(훑기가 하염없다). 12ms 는 60Hz 한 프레임(16.7ms) 안쪽이라, 훑는
/// 동안에도 화면이 계속 그려진다.
constexpr int kExplorerWalkSliceMs = 12;

/// 필터 한 글자마다 트리를 다시 훑지 않도록.
constexpr int kExplorerFilterDebounceMs = 180;

/// 도구 단추 묶음을 접고 펴는 시간.
constexpr int kExplorerActionsAnimMs = 130;

/// 탐색기가 기본으로 보여 주는 확장자.
///
/// setNameFilterDisables(false) 와 함께 쓰므로, 여기 없는 파일은 흐려지는 것이
/// 아니라 트리에서 아예 빠진다. "모든 파일 표시" 를 켜면 이 목록을 걷어낸다.
QStringList explorerNameFilters()
{
    return { QStringLiteral( "*.rst" ), QStringLiteral( "*.md" ), QStringLiteral( "*.py" ),
            QStringLiteral( "*.json" ), QStringLiteral( "*.txt" ) };
}

/// "모든 파일 표시" 가 켜져 있는가. 창을 다시 열어도 그대로여야 한다.
constexpr auto kExplorerShowAllKey = "explorer/showAllFiles";

/// 컴퓨터 루트의 드라이브 목록은 프록시가 정렬한다. QFileSystemModel 자체의
/// 정렬기는 각 드라이브의 파일 속성을 동기 조회하므로, 끊긴 원격 매핑 하나만
/// 있어도 GUI 스레드가 Windows 재연결을 기다리게 된다.
class ExplorerFileSystemModel final : public QFileSystemModel
{
public:
    using QFileSystemModel::QFileSystemModel;

    void sort( const int column, const Qt::SortOrder order ) override
    {
#ifdef Q_OS_WIN
        if( rootPath().isEmpty() )
            return;
#endif
        QFileSystemModel::sort( column, order );
    }
};

/// 위젯 하나의 포커스 진입·이탈만 알려 주는 이벤트 필터.
///
/// MainWindow 는 이미 앱 전역 eventFilter 를 걸고 있지만 그쪽은 모든 위젯의
/// 모든 이벤트가 지나는 길목이다. 입력칸 하나를 보자고 그 길을 넓히지 않는다.
/// 신호 대신 콜백을 쓰므로 Q_OBJECT 도, moc 도 필요 없다.
class FocusWatcher final : public QObject
{
public:
    FocusWatcher( QWidget* target, std::function< void( bool ) > onFocusChanged )
        : QObject( target )
        , onFocusChanged_( std::move( onFocusChanged ) )
    {
        target->installEventFilter( this );
    }

protected:
    bool eventFilter( QObject* watched, QEvent* event ) override
    {
        if( event != nullptr && onFocusChanged_ )
        {
            if( event->type() == QEvent::FocusIn )
                onFocusChanged_( true );
            else if( event->type() == QEvent::FocusOut )
                onFocusChanged_( false );
        }
        return QObject::eventFilter( watched, event );
    }

private:
    std::function< void( bool ) > onFocusChanged_;
};

/// 파일 이름으로 쓸 수 있는가.
///
/// 경로 구분자와 Windows 가 금지하는 글자를 막는다. `..` 을 막는 것이 핵심이다 —
/// 사용자가 고른 폴더 밖에 파일을 만드는 길이 되기 때문이다.
bool isSafeEntryName( const QString& name )
{
    if( name.isEmpty() || name == QLatin1String( "." ) || name == QLatin1String( ".." ) )
        return false;
    if( name.endsWith( QLatin1Char( ' ' ) ) || name.endsWith( QLatin1Char( '.' ) ) )
        return false;   // Windows 가 조용히 잘라낸다

    static const QString forbidden = QStringLiteral( R"(<>:"/\|?*)" );
    for( const QChar ch : name )
    {
        if( ch.unicode() < 0x20 || forbidden.contains( ch ) )
            return false;
    }
    return true;
}

/// 입력칸 앞머리의 돋보기. 이미 붙어 있으면 아이콘만 갈아 끼운다.
///
/// 테마가 바뀔 때마다 액션을 새로 붙이면 돋보기가 쌓인다.
void setSearchIcon( QLineEdit* edit, const QIcon& icon )
{
    if( edit == nullptr )
        return;

    const QList< QAction* > existing = edit->actions();
    for( QAction* action : existing )
    {
        if( action->property( "mrstSearchIcon" ).toBool() )
        {
            action->setIcon( icon );
            return;
        }
    }

    QAction* action = edit->addAction( icon, QLineEdit::LeadingPosition );
    action->setProperty( "mrstSearchIcon", true );
}

}  // namespace

void MainWindow::setupExplorerPanel()
{
    QTreeView* tree = Ui.treLeftSideFolterTree;
    if( tree == nullptr )
        return;

    treLeftFolderTreeModel_ = new ExplorerFileSystemModel( this );
    treLeftFolderTreeModel_->setRootPath( QString{} );
    treLeftFolderTreeModel_->setNameFilterDisables( false );

    explorerProxy_ = new mrst::FileTreeFilterProxy( this );
    explorerProxy_->setSourceModel( treLeftFolderTreeModel_ );

    tree->setModel( explorerProxy_ );
    tree->setIndentation( 15 );
    for( int column = 1; column < explorerProxy_->columnCount(); ++column )
        tree->header()->hideSection( column );

    // 헤더의 화살표를 살린다. .ui 는 headerShowSortIndicator 만 켜 두었는데 그것은
    // "그린다" 는 뜻일 뿐이라, 눌러도 아무 일이 없는 화살표가 오래 떠 있었다.
    // 정렬 규칙(폴더 먼저, 숫자는 수로)은 프록시의 lessThan 이 정한다.
    tree->setSortingEnabled( true );
    tree->header()->setSectionsClickable( true );
    tree->header()->setSortIndicatorShown( true );
    tree->sortByColumn( 0, Qt::AscendingOrder );

    connect( tree, &QTreeView::customContextMenuRequested, this,
            &MainWindow::onExplorerContextMenu );
    connect( tree, &QTreeView::doubleClicked, this, [ this ]( const QModelIndex& index ) {
        const QFileInfo info = explorerFileInfo( index );
        if( info.isFile() )
            openFile( info.absoluteFilePath() );
    } );

    explorerFilterDebounce_ = new QTimer( this );
    explorerFilterDebounce_->setSingleShot( true );
    explorerFilterDebounce_->setInterval( kExplorerFilterDebounceMs );
    connect( explorerFilterDebounce_, &QTimer::timeout, this, &MainWindow::refreshExplorerFilter );

    // 트리를 훑는 일감을 조금씩 비운다. 간격 0 은 "지금 밀려 있는 이벤트를 다
    // 처리한 뒤" 라는 뜻이다 — 그림도 그려지고 키 입력도 들어온다.
    explorerWalkTimer_ = new QTimer( this );
    explorerWalkTimer_->setSingleShot( true );
    explorerWalkTimer_->setInterval( 0 );
    connect( explorerWalkTimer_, &QTimer::timeout, this, &MainWindow::stepExplorerFilterWalk );

    if( Ui.edtExplorerFilter != nullptr )
    {
        connect( Ui.edtExplorerFilter, &QLineEdit::textChanged, this,
                [ this ] { explorerFilterDebounce_->start(); } );
        // 포커스가 오면 단추 묶음을 접어 입력칸에 줄 전체를 내준다.
        new FocusWatcher( Ui.edtExplorerFilter,
                         [ this ]( const bool focused ) { setExplorerActionsCollapsed( focused ); } );
    }

    // 폴더를 뒤늦게 읽으면 그 안까지 걸러 볼 수 있게 된다. **그 폴더만** 다시
    // 본다 — 예전에는 여기서 트리 전체를 처음부터 다시 훑었고, 큰 워크스페이스는
    // 폴더가 읽힐 때마다 그 일을 되풀이하다 멎었다.
    connect( treLeftFolderTreeModel_, &QFileSystemModel::directoryLoaded, this,
            [ this ]( const QString& path ) {
                if( explorerProxy_ == nullptr || !explorerProxy_->isFiltering() )
                    return;
                queueExplorerDirectory( treLeftFolderTreeModel_->index( path ) );
            } );

    for( QToolButton* button : { Ui.btnExplorerShowAll, Ui.btnExplorerNewFile,
                                Ui.btnExplorerNewFolder, Ui.btnExplorerRename,
                                Ui.btnExplorerDelete } )
    {
        if( button == nullptr )
            continue;
        button->setToolButtonStyle( Qt::ToolButtonIconOnly );
        button->setIconSize( QSize( 16, 16 ) );
        // 탭 순서에서 빠진다. 필터칸에서 Tab 을 치면 트리로 가야지 단추 다섯을
        // 지나가야 하는 것이 아니다. 포커스를 받지 않으므로 단추를 눌러도
        // 필터칸이 포커스를 잃지 않는다는 뜻이기도 하다.
        button->setFocusPolicy( Qt::NoFocus );
    }

    connect( Ui.btnExplorerNewFile, &QToolButton::clicked, this, &MainWindow::onExplorerNewFile );
    connect( Ui.btnExplorerNewFolder, &QToolButton::clicked, this, &MainWindow::onExplorerNewFolder );
    connect( Ui.btnExplorerRename, &QToolButton::clicked, this, &MainWindow::onExplorerRename );
    connect( Ui.btnExplorerDelete, &QToolButton::clicked, this, &MainWindow::onExplorerDelete );
    if( Ui.btnExplorerShowAll != nullptr )
    {
        connect( Ui.btnExplorerShowAll, &QToolButton::toggled, this,
                &MainWindow::setExplorerShowAllFiles );
    }

    // 지난번에 켜 두었으면 그대로 켠 채로 연다. 확장자 필터를 세우는 것도
    // 여기서 함께 한다 — setExplorerShowAllFiles 가 두 경우를 모두 다룬다.
    setExplorerShowAllFiles( AppSettings().value( kExplorerShowAllKey, false ).toBool() );

    applyExplorerIcons();
    retranslateExplorerPanel();
}

void MainWindow::setExplorerShowAllFiles( const bool showAll )
{
    if( treLeftFolderTreeModel_ == nullptr )
        return;

    // 이름 필터를 비우면 QFileSystemModel 이 전부 내보낸다. 흐리게 만드는 것이
    // 아니라 아예 내보내지 않는 설정이므로(setNameFilterDisables(false)) 목록을
    // 갈아 끼우는 것으로 충분하다.
    treLeftFolderTreeModel_->setNameFilters( showAll ? QStringList{} : explorerNameFilters() );

    if( Ui.btnExplorerShowAll != nullptr && Ui.btnExplorerShowAll->isChecked() != showAll )
    {
        const QSignalBlocker blocker( Ui.btnExplorerShowAll );
        Ui.btnExplorerShowAll->setChecked( showAll );
    }

    AppSettings().setValue( kExplorerShowAllKey, showAll );

    // 보이는 파일이 달라졌으니 필터도 다시 판정해야 한다. 켜져 있을 때만 —
    // 꺼져 있으면 프록시가 전부 통과시키므로 다시 볼 것이 없다.
    if( explorerProxy_ != nullptr && explorerProxy_->isFiltering() )
        refreshExplorerFilter();
}

void MainWindow::setExplorerActionsCollapsed( const bool collapsed )
{
    QWidget* actions = Ui.pnlExplorerActions;
    if( actions == nullptr )
        return;

    // sizeHint() 는 레이아웃이 계산하므로 maximumWidth 를 0 으로 눌러 둔 동안에도
    // 제 너비를 돌려준다. 접기 전에 재어 둘 필요가 없다.
    const int target = collapsed ? 0 : actions->sizeHint().width();

    if( explorerActionsAnimation_ == nullptr )
    {
        explorerActionsAnimation_ = new QPropertyAnimation( actions, "maximumWidth", this );
        explorerActionsAnimation_->setDuration( kExplorerActionsAnimMs );
        explorerActionsAnimation_->setEasingCurve( QEasingCurve::InOutQuad );
        connect( explorerActionsAnimation_, &QPropertyAnimation::finished, this, [ this ] {
            // 다 펴졌으면 상한을 풀어 준다. 재어 둔 값에 묶어 두면 테마나 화면
            // 배율이 바뀌어 아이콘이 커졌을 때 단추가 잘린다.
            if( Ui.pnlExplorerActions != nullptr
                && explorerActionsAnimation_->endValue().toInt() > 0 )
            {
                Ui.pnlExplorerActions->setMaximumWidth( QWIDGETSIZE_MAX );
            }
        } );
    }
    else if( explorerActionsAnimation_->state() == QAbstractAnimation::Running
             && explorerActionsAnimation_->endValue().toInt() == target )
    {
        return;   // 이미 그리로 가는 중이다
    }

    explorerActionsAnimation_->stop();
    explorerActionsAnimation_->setStartValue( actions->width() );
    explorerActionsAnimation_->setEndValue( target );
    explorerActionsAnimation_->start();
}

void MainWindow::applyExplorerIcons()
{
    if( Ui.btnExplorerNewFile == nullptr )
        return;

    // 팔레트가 곧 테마다. 색을 상수로 두지 않았으므로 라이트/다크가 저절로 갈린다.
    const QPalette palette = Ui.btnExplorerNewFile->palette();
    Ui.btnExplorerNewFile->setIcon( mrst::panelicons::newFile( palette ) );
    Ui.btnExplorerNewFolder->setIcon( mrst::panelicons::newFolder( palette ) );
    Ui.btnExplorerRename->setIcon( mrst::panelicons::rename( palette ) );
    Ui.btnExplorerDelete->setIcon( mrst::panelicons::remove( palette ) );
    if( Ui.btnExplorerShowAll != nullptr )
        Ui.btnExplorerShowAll->setIcon( mrst::panelicons::showAllFiles( palette ) );

    const QIcon magnifier = mrst::panelicons::filter( palette );
    setSearchIcon( Ui.edtExplorerFilter, magnifier );
    setSearchIcon( Ui.edtOutlineDocumentFilter, magnifier );
    setSearchIcon( Ui.edtOutlineProjectFilter, magnifier );
}

void MainWindow::retranslateExplorerPanel()
{
    if( Ui.edtExplorerFilter != nullptr )
    {
        Ui.edtExplorerFilter->setPlaceholderText( tr( "필터 (부분 일치, * ? 가능)" ) );
        Ui.edtExplorerFilter->setToolTip(
            tr( "이름의 일부를 치면 걸러집니다. `*` 나 `?` 를 넣으면 와일드카드로 봅니다." ) );
    }

    const auto label = []( QToolButton* button, const QString& text, const QString& tip ) {
        if( button == nullptr )
            return;
        button->setText( text );
        button->setToolTip( tip );
    };
    label( Ui.btnExplorerShowAll, tr( "모든 파일 표시" ),
          tr( "지원하지 않는 확장자까지 트리에 보여 줍니다." ) );
    label( Ui.btnExplorerNewFile, tr( "새 파일" ), tr( "새 파일 만들기" ) );
    label( Ui.btnExplorerNewFolder, tr( "새 폴더" ), tr( "새 폴더 만들기" ) );
    label( Ui.btnExplorerRename, tr( "이름 바꾸기" ), tr( "고른 항목의 이름 바꾸기" ) );
    label( Ui.btnExplorerDelete, tr( "삭제" ), tr( "고른 항목 삭제" ) );
}

QFileInfo MainWindow::explorerFileInfo( const QModelIndex& proxyIndex ) const
{
    if( !proxyIndex.isValid() || explorerProxy_ == nullptr || treLeftFolderTreeModel_ == nullptr )
        return {};
    return treLeftFolderTreeModel_->fileInfo( explorerProxy_->mapToSource( proxyIndex ) );
}

QFileInfo MainWindow::explorerCurrentFileInfo() const
{
    return Ui.treLeftSideFolterTree == nullptr
               ? QFileInfo{}
               : explorerFileInfo( Ui.treLeftSideFolterTree->currentIndex() );
}

QString MainWindow::explorerTargetDirectory() const
{
    const QFileInfo info = explorerCurrentFileInfo();
    if( info.isDir() )
        return info.absoluteFilePath();
    if( info.isFile() )
        return info.absolutePath();

    // 고른 것이 없으면 워크스페이스 뿌리에 만든다.
    if( explorerProxy_ == nullptr || treLeftFolderTreeModel_ == nullptr )
        return {};
    return treLeftFolderTreeModel_->filePath(
        explorerProxy_->mapToSource( Ui.treLeftSideFolterTree->rootIndex() ) );
}

// ── 필터 ──────────────────────────────────────────────────

void MainWindow::refreshExplorerFilter()
{
    QTreeView* tree = Ui.treLeftSideFolterTree;
    if( tree == nullptr || explorerProxy_ == nullptr || Ui.edtExplorerFilter == nullptr )
        return;

    const bool wasFiltering = explorerProxy_->isFiltering();
    if( !wasFiltering )
    {
        // 필터를 걸기 직전의 펼침 상태를 기억해 둔다. 필터를 지웠을 때 트리가
        // 통째로 펼쳐진 채 남으면 그 전에 보고 있던 자리를 다시 찾아야 한다.
        explorerExpandedBeforeFilter_ = expandedExplorerPaths();
    }

    // 지난 문구를 좇던 일감을 버린다. 남겨 두면 이제 아무도 찾지 않는 폴더를
    // 계속 읽는다.
    stopExplorerFilterWalk();

    explorerProxy_->setFilterText( Ui.edtExplorerFilter->text() );

    if( explorerProxy_->isFiltering() )
    {
        beginExplorerFilterWalk();
        return;
    }

    if( wasFiltering )
    {
        // 접었다 되돌리는 두 걸음을 한 번에 그린다. 그러지 않으면 트리가 통째로
        // 접혔다 펴지는 것이 그대로 보인다.
        tree->setUpdatesEnabled( false );
        tree->collapseAll();
        restoreExplorerExpansion( explorerExpandedBeforeFilter_ );
        tree->setUpdatesEnabled( true );
        explorerExpandedBeforeFilter_.clear();
    }
}

void MainWindow::beginExplorerFilterWalk()
{
    QTreeView* tree = Ui.treLeftSideFolterTree;
    if( tree == nullptr || explorerProxy_ == nullptr || treLeftFolderTreeModel_ == nullptr )
        return;

    // 워크스페이스가 없으면 훑지 않는다. 그때 트리의 뿌리는 모델의 최상위,
    // 곧 이 컴퓨터의 드라이브 전부다 — 거기서 시작하면 한 글자에 디스크를 훑는다.
    const QModelIndex root = tree->rootIndex();
    if( !root.isValid() )
        return;

    explorerWalkBudget_ = kExplorerWalkBudget;
    queueExplorerDirectory( explorerProxy_->mapToSource( root ) );
}

void MainWindow::queueExplorerDirectory( const QModelIndex& sourceIndex )
{
    if( !sourceIndex.isValid() || explorerWalkBudget_ <= 0 || explorerWalkTimer_ == nullptr )
        return;

    --explorerWalkBudget_;
    explorerWalkQueue_.append( QPersistentModelIndex( sourceIndex ) );
    explorerWalkTimer_->start();
}

void MainWindow::stepExplorerFilterWalk()
{
    QTreeView* tree = Ui.treLeftSideFolterTree;
    if( tree == nullptr || explorerProxy_ == nullptr || treLeftFolderTreeModel_ == nullptr
        || !explorerProxy_->isFiltering() )
    {
        stopExplorerFilterWalk();
        return;
    }

    const QModelIndex treeRoot = tree->rootIndex();

    QElapsedTimer slice;
    slice.start();
    while( !explorerWalkQueue_.isEmpty() && slice.elapsed() < kExplorerWalkSliceMs )
    {
        const QModelIndex source = explorerWalkQueue_.takeFirst();
        if( !source.isValid() )
            continue;

        // 아직 안 읽은 폴더는 **읽으라고만** 한다. 뷰를 펼치지 않는 것이 요점이다.
        //
        // 읽고 나서야 필터를 통과하는지 알 수 있는데, 먼저 펼쳐 두면 통과하지
        // 못한 폴더가 펼쳐졌다가 사라지는 것이 그대로 보인다 — "디렉터리가
        // 펼쳐졌다 접힌다" 는 증상이 그것이었다. 다 읽히면 directoryLoaded 가
        // 이 폴더를 다시 큐에 넣고, 그때 아래로 내려간다.
        if( treLeftFolderTreeModel_->canFetchMore( source ) )
        {
            treLeftFolderTreeModel_->fetchMore( source );
            continue;
        }

        const QModelIndex parent = explorerProxy_->mapFromSource( source );
        if( !parent.isValid() )
            continue;   // 그새 필터에 걸러졌다

        if( parent != treeRoot )
            tree->expand( parent );

        const int rows = explorerProxy_->rowCount( parent );
        for( int row = 0; row < rows; ++row )
        {
            const QModelIndex child = explorerProxy_->index( row, 0, parent );
            // hasChildren() 은 QFileSystemModel 에서 그대로 isDir() 이다.
            // 파일은 여기서 걸러진다.
            if( !explorerProxy_->hasChildren( child ) )
                continue;
            queueExplorerDirectory( explorerProxy_->mapToSource( child ) );
        }
    }

    if( !explorerWalkQueue_.isEmpty() )
        explorerWalkTimer_->start();
}

void MainWindow::stopExplorerFilterWalk()
{
    if( explorerWalkTimer_ != nullptr )
        explorerWalkTimer_->stop();
    explorerWalkQueue_.clear();
    explorerWalkBudget_ = 0;
}

QStringList MainWindow::expandedExplorerPaths() const
{
    QTreeView* tree = Ui.treLeftSideFolterTree;
    if( tree == nullptr || explorerProxy_ == nullptr || treLeftFolderTreeModel_ == nullptr )
        return {};

    QStringList paths;
    const std::function< void( const QModelIndex& ) > walk = [ & ]( const QModelIndex& parent ) {
        const int rows = explorerProxy_->rowCount( parent );
        for( int row = 0; row < rows; ++row )
        {
            const QModelIndex index = explorerProxy_->index( row, 0, parent );
            if( !tree->isExpanded( index ) )
                continue;
            paths << treLeftFolderTreeModel_->filePath( explorerProxy_->mapToSource( index ) );
            walk( index );
        }
    };
    walk( tree->rootIndex() );
    return paths;
}

void MainWindow::restoreExplorerExpansion( const QStringList& paths )
{
    QTreeView* tree = Ui.treLeftSideFolterTree;
    if( tree == nullptr || explorerProxy_ == nullptr || treLeftFolderTreeModel_ == nullptr )
        return;

    // 얕은 것부터 펼쳐야 한다 — 부모가 닫혀 있으면 자식 인덱스가 없다.
    // expandedExplorerPaths() 가 전위 순회로 모으므로 순서가 이미 그렇다.
    for( const QString& path : paths )
    {
        const QModelIndex source = treLeftFolderTreeModel_->index( path );
        if( !source.isValid() )
            continue;
        const QModelIndex index = explorerProxy_->mapFromSource( source );
        if( index.isValid() )
            tree->expand( index );
    }
}

void MainWindow::selectExplorerPath( const QString& path )
{
    QTreeView* tree = Ui.treLeftSideFolterTree;
    if( tree == nullptr || explorerProxy_ == nullptr || treLeftFolderTreeModel_ == nullptr )
        return;

    const QModelIndex source = treLeftFolderTreeModel_->index( path );
    if( !source.isValid() )
        return;   // 모델이 아직 그 폴더를 읽지 않았다. 최선을 다한 것으로 둔다

    const QModelIndex index = explorerProxy_->mapFromSource( source );
    if( !index.isValid() )
        return;   // 지금 필터에 걸리지 않는다

    tree->setCurrentIndex( index );
    tree->scrollTo( index );
}

// ── 컨텍스트 메뉴 ─────────────────────────────────────────

QString MainWindow::projectIdForDirectory( const QString& dirPath ) const
{
    if( controller_ == nullptr || dirPath.isEmpty() )
        return {};

    mrst::ProjectRegistry* registry = controller_->projectRegistry();
    if( registry == nullptr )
        return {};

    const QString wanted = QDir( dirPath ).absolutePath();
    for( const mrst::SphinxProject& project : registry->projects() )
    {
        const QString root = QDir( mrst::toQString( project.rootPath ) ).absolutePath();
        if( root.compare( wanted, Qt::CaseInsensitive ) == 0 )
            return QString::fromStdWString( project.projectId );
    }
    return {};
}

void MainWindow::onExplorerContextMenu( const QPoint& pos )
{
    QTreeView* tree = Ui.treLeftSideFolterTree;
    if( tree == nullptr )
        return;

    const QModelIndex index = tree->indexAt( pos );
    if( index.isValid() )
        tree->setCurrentIndex( index );

    const QFileInfo info = explorerFileInfo( index );
    QMenu           menu( tree );

    // 빌드는 **실제** Sphinx 프로젝트의 루트에서만 연다. 가상 프로젝트는 산출물이
    // 임시 디렉터리라 사용자가 고른 자리에 놓을 것이 애초에 없다.
    if( info.isDir() )
    {
        const QString root = info.absoluteFilePath();
        if( const QString projectId = projectIdForDirectory( root ); !projectId.isEmpty() )
        {
            QAction* build = menu.addAction( tr( "빌드(&B)…" ) );
            build->setEnabled( controller_ != nullptr && !controller_->isProjectBuildRunning() );
            connect( build, &QAction::triggered, this,
                    [ this, projectId, root ] { onExplorerBuild( projectId, root ); } );
            menu.addSeparator();
        }
    }

    QAction* newFile = menu.addAction( tr( "새 파일(&N)…" ) );
    QAction* newFolder = menu.addAction( tr( "새 폴더(&F)…" ) );
    menu.addSeparator();
    QAction* rename = menu.addAction( tr( "이름 바꾸기(&M)…" ) );
    QAction* remove = menu.addAction( tr( "삭제(&D)…" ) );
    menu.addSeparator();
    QAction* reveal = menu.addAction( tr( "파일 관리자에서 보기(&E)" ) );
    menu.addSeparator();

    // 도구 줄에도 같은 토글이 있지만, 필터칸이 포커스를 쥐고 있으면 그 단추
    // 묶음이 접혀 있어 누를 수 없다. 여기에 하나 더 두어 길을 막지 않는다.
    QAction* showAll = menu.addAction( tr( "모든 파일 표시(&A)" ) );
    showAll->setCheckable( true );
    showAll->setChecked( treLeftFolderTreeModel_ != nullptr
                        && treLeftFolderTreeModel_->nameFilters().isEmpty() );
    connect( showAll, &QAction::toggled, this, &MainWindow::setExplorerShowAllFiles );

    const bool hasTarget = info.exists();
    rename->setEnabled( hasTarget );
    remove->setEnabled( hasTarget );
    reveal->setEnabled( hasTarget );

    connect( newFile, &QAction::triggered, this, &MainWindow::onExplorerNewFile );
    connect( newFolder, &QAction::triggered, this, &MainWindow::onExplorerNewFolder );
    connect( rename, &QAction::triggered, this, &MainWindow::onExplorerRename );
    connect( remove, &QAction::triggered, this, &MainWindow::onExplorerDelete );
    connect( reveal, &QAction::triggered, this,
            [ this, path = info.absoluteFilePath() ] { revealInFileManager( path ); } );

    menu.exec( tree->viewport()->mapToGlobal( pos ) );
}

// ── 파일 조작 ─────────────────────────────────────────────

void MainWindow::onExplorerNewFile()
{
    const QString directory = explorerTargetDirectory();
    if( directory.isEmpty() )
    {
        QMessageBox::information( this, tr( "새 파일" ), tr( "먼저 워크스페이스를 여십시오." ) );
        return;
    }

    bool          accepted = false;
    const QString name = QInputDialog::getText( this, tr( "새 파일" ),
                                               tr( "%1 에 만들 파일 이름:" ).arg( directory ),
                                               QLineEdit::Normal,
                                               tr( "새 문서.rst" ), &accepted ).trimmed();
    if( !accepted || name.isEmpty() )
        return;
    if( !isSafeEntryName( name ) )
    {
        QMessageBox::warning( this, tr( "새 파일" ), tr( "파일 이름으로 쓸 수 없는 글자가 있습니다." ) );
        return;
    }

    const QString path = QDir( directory ).absoluteFilePath( name );
    if( QFileInfo::exists( path ) )
    {
        QMessageBox::warning( this, tr( "새 파일" ), tr( "같은 이름이 이미 있습니다:\n%1" ).arg( path ) );
        return;
    }

    QFile file( path );
    if( !file.open( QIODevice::WriteOnly ) )
    {
        QMessageBox::warning( this, tr( "새 파일" ),
                             tr( "파일을 만들 수 없습니다:\n%1" ).arg( file.errorString() ) );
        return;
    }
    file.close();

    appendLog( tr( "새 파일: %1" ).arg( path ) );
    openFile( path );
    selectExplorerPath( path );
}

void MainWindow::onExplorerNewFolder()
{
    const QString directory = explorerTargetDirectory();
    if( directory.isEmpty() )
    {
        QMessageBox::information( this, tr( "새 폴더" ), tr( "먼저 워크스페이스를 여십시오." ) );
        return;
    }

    bool          accepted = false;
    const QString name = QInputDialog::getText( this, tr( "새 폴더" ),
                                               tr( "%1 에 만들 폴더 이름:" ).arg( directory ),
                                               QLineEdit::Normal, tr( "새 폴더" ), &accepted ).trimmed();
    if( !accepted || name.isEmpty() )
        return;
    if( !isSafeEntryName( name ) )
    {
        QMessageBox::warning( this, tr( "새 폴더" ), tr( "폴더 이름으로 쓸 수 없는 글자가 있습니다." ) );
        return;
    }

    const QString path = QDir( directory ).absoluteFilePath( name );
    if( QFileInfo::exists( path ) )
    {
        QMessageBox::warning( this, tr( "새 폴더" ), tr( "같은 이름이 이미 있습니다:\n%1" ).arg( path ) );
        return;
    }
    if( !QDir().mkpath( path ) )
    {
        QMessageBox::warning( this, tr( "새 폴더" ), tr( "폴더를 만들 수 없습니다:\n%1" ).arg( path ) );
        return;
    }

    appendLog( tr( "새 폴더: %1" ).arg( path ) );
    selectExplorerPath( path );
}

void MainWindow::onExplorerRename()
{
    const QFileInfo info = explorerCurrentFileInfo();
    if( !info.exists() )
        return;

    const QString oldPath = info.absoluteFilePath();

    // 열려 있는 탭의 경로가 어긋난다. QBaseView 는 파일 경로를 밖에서 바꿀 길을
    // 주지 않으므로(setter 가 없다), 저장된 탭은 닫았다 새 이름으로 다시 연다.
    // 편집 중인 탭은 손대지 않는다 — 여기서 닫으면 저장 여부를 묻는 대화상자가
    // 이름 바꾸기 한복판에 끼어든다.
    QStringList affected;
    QStringList modified;
    for( int index = 0; m_tabWidget != nullptr && index < m_tabWidget->count(); ++index )
    {
        auto* view = qobject_cast< QBaseView* >( m_tabWidget->widget( index ) );
        if( view == nullptr )
            continue;
        const QString path = view->currentFilePath();
        if( path.isEmpty() )
            continue;
        const bool underTarget = info.isDir()
                                     ? path.startsWith( oldPath + QLatin1Char( '/' ), Qt::CaseInsensitive )
                                     : path.compare( oldPath, Qt::CaseInsensitive ) == 0;
        if( !underTarget )
            continue;
        ( view->isModified() ? modified : affected ) << path;
    }

    if( !modified.isEmpty() )
    {
        QMessageBox::warning( this, tr( "이름 바꾸기" ),
                             tr( "저장하지 않은 편집이 있는 탭이 있습니다. 먼저 저장하거나 닫으십시오:\n%1" )
                                 .arg( modified.join( QStringLiteral( "\n" ) ) ) );
        return;
    }

    bool          accepted = false;
    const QString name = QInputDialog::getText( this, tr( "이름 바꾸기" ), tr( "새 이름:" ),
                                               QLineEdit::Normal, info.fileName(), &accepted ).trimmed();
    if( !accepted || name.isEmpty() || name == info.fileName() )
        return;
    if( !isSafeEntryName( name ) )
    {
        QMessageBox::warning( this, tr( "이름 바꾸기" ), tr( "이름으로 쓸 수 없는 글자가 있습니다." ) );
        return;
    }

    const QString newPath = QDir( info.absolutePath() ).absoluteFilePath( name );
    if( QFileInfo::exists( newPath ) )
    {
        QMessageBox::warning( this, tr( "이름 바꾸기" ),
                             tr( "같은 이름이 이미 있습니다:\n%1" ).arg( newPath ) );
        return;
    }

    // 딸린 탭을 먼저 닫는다. 파일이 열려 있으면 Windows 가 이름 바꾸기를 막는다.
    for( const QString& path : std::as_const( affected ) )
    {
        if( QTextView* view = textViewForPath( path ); view != nullptr )
        {
            if( const int index = m_tabWidget->indexOf( view ); index >= 0 )
                onCloseTab( index );
        }
    }

    if( !QDir().rename( oldPath, newPath ) )
    {
        QMessageBox::warning( this, tr( "이름 바꾸기" ),
                             tr( "이름을 바꿀 수 없습니다:\n%1" ).arg( oldPath ) );
        return;
    }

    appendLog( tr( "이름 바꾸기: %1 → %2" ).arg( oldPath, newPath ) );

    // 닫아 두었던 탭을 새 경로로 되돌린다.
    for( const QString& path : std::as_const( affected ) )
    {
        QString moved = path;
        moved.replace( 0, oldPath.length(), newPath );
        openFile( moved );
    }
    selectExplorerPath( newPath );
}

void MainWindow::onExplorerDelete()
{
    const QFileInfo info = explorerCurrentFileInfo();
    if( !info.exists() )
        return;

    const QString path = info.absoluteFilePath();
    const QString question =
        info.isDir() ? tr( "폴더와 그 안의 모든 것을 지웁니다. 되돌릴 수 없습니다.\n\n%1" ).arg( path )
                     : tr( "파일을 지웁니다. 되돌릴 수 없습니다.\n\n%1" ).arg( path );
    if( QMessageBox::question( this, tr( "삭제" ), question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No ) != QMessageBox::Yes )
    {
        return;
    }

    // 열려 있는 탭은 닫지 않는다. 외부 변경 감시가 "사라졌다" 를 받아 버퍼를
    // 그대로 둔 채 수정됨으로 표시하므로, 그 버퍼가 마지막 사본으로 남는다.
    const bool ok = info.isDir() ? QDir( path ).removeRecursively() : QFile::remove( path );
    if( !ok )
    {
        QMessageBox::warning( this, tr( "삭제" ), tr( "지울 수 없습니다:\n%1" ).arg( path ) );
        return;
    }
    appendLog( tr( "삭제: %1" ).arg( path ) );
}

void MainWindow::revealInFileManager( const QString& path )
{
    if( path.isEmpty() || !QFileInfo::exists( path ) )
        return;

#ifdef Q_OS_WIN
    const QFileInfo info( path );
    const QString   native = QDir::toNativeSeparators( info.absoluteFilePath() );
    // 파일이면 그것을 고른 채로 연다. `/select,` 는 쉼표까지가 한 인자다.
    const QStringList arguments = info.isDir()
                                      ? QStringList{ native }
                                      : QStringList{ QStringLiteral( "/select," ) + native };
    QProcess::startDetached( QStringLiteral( "explorer.exe" ), arguments );
#else
    const QFileInfo info( path );
    QDesktopServices::openUrl(
        QUrl::fromLocalFile( info.isDir() ? info.absoluteFilePath() : info.absolutePath() ) );
#endif
}

// ── 빌드 ──────────────────────────────────────────────────

void MainWindow::onExplorerBuild( const QString& projectId, const QString& projectRoot )
{
    if( controller_ == nullptr )
        return;
    if( controller_->isProjectBuildRunning() )
    {
        QMessageBox::information( this, tr( "빌드" ),
                                 tr( "빌드가 이미 돌고 있습니다. 끝난 뒤에 다시 요청하십시오." ) );
        return;
    }

    QSphinxBuildDialog dialog( projectId, projectRoot, this );
    if( dialog.exec() != QDialog::Accepted )
        return;

    const bool openWhenDone = dialog.openWhenDone();

    // 한 번만 받는다. 연결을 남겨 두면 다음 빌드에서도 탐색기가 열린다.
    const QMetaObject::Connection once =
        connect( controller_, &mrst::WorkspaceController::projectBuildFinished, this,
                [ this, openWhenDone ]( const QString&, const QString&,
                                        const QString& resultDirectory, const bool ok,
                                        const bool cancelled ) {
                    if( cancelled )
                        return;
                    if( !ok )
                    {
                        QMessageBox::warning( this, tr( "빌드" ),
                                             tr( "빌드에 실패했습니다. 로그 탭을 확인하십시오." ) );
                        return;
                    }
                    showTransientStatus( tr( "빌드 완료: %1" ).arg( resultDirectory ), 8000 );
                    if( openWhenDone )
                        revealInFileManager( resultDirectory );
                },
                Qt::SingleShotConnection );

    // 진행 상황이 로그로 흐르므로 그 탭을 앞으로 꺼내 준다.
    if( dockLog_ != nullptr )
        dockLog_->setAsCurrentTab();

    // 시작하지 못했으면 연결을 거둔다. 남겨 두면 **다음** 빌드가 끝날 때
    // 이번에 고른 "탐색기로 열기" 로 반응한다.
    if( !controller_->buildProject( projectId, dialog.builder(), dialog.outputDirectory() ) )
        disconnect( once );
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

/// 이 심볼들과 그 후손을 모두 센다. 깊이 제한으로 트리에서 빠진 개수를 알릴 때 쓴다.
int countOutlineSymbols( const QVector< mrst::OutlineSymbol >& symbols )
{
    int count = 0;
    for( const mrst::OutlineSymbol& symbol : symbols )
        count += 1 + countOutlineSymbols( symbol.children );
    return count;
}

/// remainingDepth 는 여기서 더 내려갈 수 있는 단계 수다. 0 이면 심볼을 올리지
/// 않고, 대신 부모 줄에 말줄임표와 툴팁을 남긴다 — 표시 없이 자르면 그 자리가
/// 문서의 마지막 단계인 것처럼 읽힌다(프로젝트 개요의 "… n개 문서 생략" 과
/// 같은 이유다).
void addOutlineSymbols( QTreeWidgetItem* parent, QTreeWidget* tree,
                        const QVector< mrst::OutlineSymbol >& symbols, int remainingDepth )
{
    if( symbols.isEmpty() )
        return;
    if( remainingDepth <= 0 )
    {
        if( parent == nullptr )
            return;                     // 최상위 호출은 언제나 1 이상을 받는다
        parent->setText( 0, parent->text( 0 ) + QStringLiteral( " …" ) );
        parent->setToolTip(
            0, MainWindow::tr( "하위 %n개 항목이 개요 깊이 설정에 걸려 빠졌습니다.", nullptr,
                               countOutlineSymbols( symbols ) ) );
        return;
    }

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

        addOutlineSymbols( item, tree, symbol.children, remainingDepth - 1 );
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

/// 설정에 적힌 개요 깊이. 0 은 "제한하지 않음" 이다
/// (preview/unsavedEditMaxReadMs 의 관례와 같다). 음수는 ini 를 손으로 고친
/// 경우이므로 기본값으로 본다.
int outlineDepthSetting()
{
    const int depth =
        AppSettings().value( QStringLiteral( "preview/outlineMaxDepth" ), 3 ).toInt();
    return depth >= 0 ? depth : 3;
}

/// addOutlineSymbols 에 넘길 예산. "제한하지 않음" 을 사실상 무한으로 바꾼다.
int outlineDepthBudget( const int maxDepth )
{
    return maxDepth > 0 ? maxDepth : std::numeric_limits< int >::max();
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
    // 필터는 프록시가 아니라 항목 감추기로 한다. QTreeWidget 은 자기 모델을
    // 밖에 내주지 않아(setModel 이 private 이다) 프록시를 끼울 자리가 없다.
    // 화면에 보이는 결과는 같다 — 자손이 걸리면 조상도 남고 그 가지가 펼쳐진다.
    for( QLineEdit* edit : { Ui.edtOutlineDocumentFilter, Ui.edtOutlineProjectFilter } )
    {
        if( edit == nullptr )
            continue;
        edit->setPlaceholderText( tr( "필터 (부분 일치)" ) );
        connect( edit, &QLineEdit::textChanged, this, &MainWindow::refreshOutlineFilters );
    }

    outlineMaxDepth_ = outlineDepthSetting();
    setOutlinePlaceholder( Ui.treOutlineDocument, tr( "열린 문서가 없습니다." ), "noDocument" );
    setOutlinePlaceholder( Ui.treOutlineProject, tr( "활성 Sphinx 프로젝트가 없습니다." ), "noProject" );

    // 받은 개요는 그대로 쥐고 있는다. 깊이 설정이 바뀌면 이것으로 다시 그린다.
    connect( controller_, &mrst::WorkspaceController::documentOutlineReady, this,
            [this]( const QString&, const QVector< mrst::OutlineSymbol >& symbols ) {
                outlineDocumentSymbols_ = symbols;
                if( symbols.isEmpty() )
                {
                    setOutlinePlaceholder( Ui.treOutlineDocument, tr( "문서 심볼이 없습니다." ),
                                          "noSymbols" );
                    return;
                }
                redrawDocumentOutlineTree();
            } );

    connect( controller_, &mrst::WorkspaceController::projectOutlineReady, this,
            [this]( const QString&, const QVector< mrst::OutlineDocumentEntry >& documents,
                    const int truncated ) {
                outlineProjectDocuments_ = documents;
                outlineProjectTruncated_ = truncated;
                if( documents.isEmpty() )
                {
                    setOutlinePlaceholder( Ui.treOutlineProject,
                                          tr( "활성 프로젝트에 문서가 없습니다." ), "noProjectDocs" );
                    return;
                }
                redrawProjectOutlineTree();
            } );

    connect( controller_, &mrst::WorkspaceController::outlineCleared, this,
            [this]( const QString& reason ) {
                outlineDocumentSymbols_.clear();
                outlineProjectDocuments_.clear();
                outlineProjectTruncated_ = 0;
                setOutlinePlaceholder( Ui.treOutlineDocument, reason );
                setOutlinePlaceholder( Ui.treOutlineProject,
                                       tr( "활성 Sphinx 프로젝트가 없습니다." ), "noProject" );
            } );

    // 어느 탭을 앞에 둘지는 활성 문서의 소속에 달렸다. 이 신호는 소속이 정해진
    // 뒤에만 오므로(스캔 중에는 오지 않는다) 프로젝트 문서를 단독 문서로 오해해
    // 탭을 흔드는 일이 없다. documentOutlineReady 는 트리거로 쓸 수 없다 —
    // 편집 디바운스와 LSP 응답으로 한 문서에 여러 번 온다.
    connect( controller_, &mrst::WorkspaceController::activeDocumentResolved, this,
            &MainWindow::applyDefaultOutlineTab );
}

void MainWindow::redrawDocumentOutlineTree()
{
    QTreeWidget* tree = Ui.treOutlineDocument;
    if( tree == nullptr || outlineDocumentSymbols_.isEmpty() )
        return;                         // 비면 안내 문구가 떠 있는 중이다

    tree->clear();
    addOutlineSymbols( nullptr, tree, outlineDocumentSymbols_,
                      outlineDepthBudget( outlineMaxDepth_ ) );
    tree->expandToDepth( 1 );
    // 트리를 다시 만들었으니 걸려 있던 필터도 다시 걸어야 한다.
    refreshOutlineFilters();
}

void MainWindow::redrawProjectOutlineTree()
{
    QTreeWidget* tree = Ui.treOutlineProject;
    if( tree == nullptr || outlineProjectDocuments_.isEmpty() )
        return;

    const int budget = outlineDepthBudget( outlineMaxDepth_ );
    tree->clear();
    for( const mrst::OutlineDocumentEntry& document : outlineProjectDocuments_ )
    {
        auto* item = new QTreeWidgetItem( QStringList{ document.label } );
        item->setData( 0, kOutlinePathRole, document.path );
        item->setData( 0, kOutlineLineRole, 1 );
        tree->addTopLevelItem( item );

        // 문서 줄은 단계로 세지 않는다. 그래야 두 탭의 "3단계" 가 같은 것을 뜻한다 —
        // 문서 줄까지 세면 프로젝트 탭은 섹션을 한 단계 덜 보여 준다.
        if( document.symbols.isEmpty() )
            item->addChild( new QTreeWidgetItem( QStringList{ tr( "심볼 없음" ) } ) );
        else
            addOutlineSymbols( item, tree, document.symbols, budget );
    }
    if( outlineProjectTruncated_ > 0 )
    {
        tree->addTopLevelItem( new QTreeWidgetItem(
            QStringList{ tr( "… %n개 문서 생략", nullptr, outlineProjectTruncated_ ) } ) );
    }
    tree->expandToDepth( 0 );
    refreshOutlineFilters();
}

bool MainWindow::applyOutlineFilter( QTreeWidget* tree, const QString& text )
{
    if( tree == nullptr )
        return false;

    const QString needle = text.trimmed();
    const std::function< bool( QTreeWidgetItem* ) > walk = [ & ]( QTreeWidgetItem* item ) {
        bool anyChild = false;
        for( int row = 0; row < item->childCount(); ++row )
        {
            // `||` 를 앞에 두면 안 된다. 첫 자식이 걸린 순간 나머지를 걷지 않아
            // 그 형제들이 감춰진 채로 남는다.
            anyChild = walk( item->child( row ) ) || anyChild;
        }

        const bool self = needle.isEmpty() || item->text( 0 ).contains( needle, Qt::CaseInsensitive );
        const bool keep = self || anyChild;
        item->setHidden( !keep );
        if( anyChild && !needle.isEmpty() )
            item->setExpanded( true );
        return keep;
    };

    bool any = false;
    for( int row = 0; row < tree->topLevelItemCount(); ++row )
        any = walk( tree->topLevelItem( row ) ) || any;
    return any;
}

void MainWindow::refreshOutlineFilters()
{
    if( Ui.edtOutlineDocumentFilter != nullptr )
        applyOutlineFilter( Ui.treOutlineDocument, Ui.edtOutlineDocumentFilter->text() );
    if( Ui.edtOutlineProjectFilter != nullptr )
        applyOutlineFilter( Ui.treOutlineProject, Ui.edtOutlineProjectFilter->text() );
}

void MainWindow::reloadOutlineDepth()
{
    const int depth = outlineDepthSetting();
    if( depth == outlineMaxDepth_ )
        return;

    outlineMaxDepth_ = depth;
    redrawDocumentOutlineTree();
    redrawProjectOutlineTree();
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
    if( dockManager_ == nullptr || dockDiagnostics_ == nullptr )
        return;

    // 이 패널만 .ui 에 없다. 내용이 코드로 조립되기 때문이다 — 그래서 pnl*
    // 컨테이너 대신 여기서 만들어 도크에 담는다.
    auto* page = new QWidget;
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
    dockSearch_ = makeDock( "dock.search", tr( "검색" ), page );
    if( ads::CDockAreaWidget* bottomArea = dockDiagnostics_->dockAreaWidget() )
        dockManager_->addDockWidgetTabToArea( dockSearch_, bottomArea );
    else
        dockManager_->addDockWidget( ads::BottomDockWidgetArea, dockSearch_ );
    // 진단이 계속 앞에 있어야 한다. 탭을 더하면 그것이 활성 탭이 된다.
    dockDiagnostics_->setAsCurrentTab();
    // createMenus() 는 이미 지났으므로 보기 > 패널에 여기서 붙인다.
    if( dockPanelsMenu_ != nullptr )
        dockPanelsMenu_->addAction( dockSearch_->toggleViewAction() );

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

        if( isPreviewDocumentView( view ) )
        {
            const int percent = previewZoomPercentForView( view );
            const QString path = normalizeFilePath( document.path );
            if( percent == mrst::kDefaultPreviewZoomPercent )
                previewZoomPercentByPath_.remove( path );
            else
                previewZoomPercentByPath_.insert( path, percent );
        }

        if( m_tabWidget->currentIndex() == index )
            session.activeIndex = static_cast< int >( session.documents.size() );
        session.documents.push_back( document );
    }
    session.previewZoomPercentByPath = previewZoomPercentByPath_;

    // 전체 화면 도중에는 화면의 배치를 저장하지 않는다. 지금 배치는 "편집기와
    // 좌측·하단이 없음" 이라서, 그대로 저장하면 다음 실행이 **아무것도 없는
    // 창**으로 열린다. 들어올 때 기억해 둔 값을 쓴다.
    if( previewFullScreen_.active )
    {
        session.dockLayout = QString::fromLatin1( previewFullScreen_.dockState.toBase64() );
        session.previewSplitterSizes = previewFullScreen_.previewSplitSizes;
    }
    else
    {
        session.dockLayout = QString::fromLatin1( dockManager_->saveState().toBase64() );
        if( QSplitter* splitter = Ui.splitter_2 )
            session.previewSplitterSizes = splitter->sizes();
    }

    // 배치를 기억하지 않기로 했으면 창 크기도 남기지 않는다. 남겨 두면 설정을
    // 껐다 켠 사용자가 **설정을 끄기 전**의 창으로 돌아가고, 그 사이의 조작이
    // 어디로 갔는지 알 수 없게 된다.
    if( layoutRestoreEnabled() )
        session.windowGeometry = currentWindowGeometry();

    mrst::saveWorkspaceSession( session );
}

// ═══════════════════════════════════════════════════════════
// 창·패널 배치의 복원
//
// 배치는 **워크스페이스에 속한 상태**다(solWorkspaceSession.hpp 참고). 어떤 폴더는
// 좌측 탐색기를 넓게 쓰고 어떤 폴더는 프리뷰만 크게 보는 것이 자연스럽고, 그 둘을
// 전역 설정 한 벌로 묶으면 워크스페이스를 옮길 때마다 배치를 다시 잡게 된다.
// ═══════════════════════════════════════════════════════════
bool MainWindow::layoutRestoreEnabled()
{
    return AppSettings().value( QStringLiteral( "window/restoreLayout" ), true ).toBool();
}

QString MainWindow::currentWindowGeometry() const
{
    // 전체 화면 중이면 들어가기 직전 값을 쓴다. 지금 saveGeometry() 를 찍으면
    // 전체 화면 플래그까지 담겨, 다음 실행이 메뉴도 편집기도 없는 창으로 뜬다.
    const QByteArray geometry = previewFullScreen_.active
        ? previewFullScreen_.windowGeometry
        : saveGeometry();
    if( geometry.isEmpty() )
        return {};
    return QString::fromLatin1( geometry.toBase64() );
}

void MainWindow::restoreWindowGeometry( const QString& base64 )
{
    if( base64.isEmpty() || !layoutRestoreEnabled() )
        return;

    const QByteArray geometry = QByteArray::fromBase64( base64.toLatin1() );
    if( geometry.isEmpty() )
        return;

    // restoreGeometry() 는 화면 밖으로 나간 창을 스스로 화면 안으로 당겨 준다
    // (모니터를 떼고 온 경우). 실패하면 손대지 않고 false 를 돌려주므로 지금
    // 크기가 그대로 남는다.
    windowGeometryRestored_ = restoreGeometry( geometry );
}

void MainWindow::restoreWindowGeometryForLastWorkspace()
{
    if( !layoutRestoreEnabled() )
        return;

    const QString lastRoot = AppSettings().value( QStringLiteral( "workspace/lastRoot" ) ).toString();
    if( lastRoot.isEmpty() || mrst::isDisconnectedRemoteDrivePath( lastRoot )
        || !QFileInfo( lastRoot ).isDir() )
        return;

    // 세션 파일을 여기서 한 번, restoreLastSession() 에서 또 한 번 읽는다. 창
    // 크기는 **show() 보다 먼저** 정해져야 창이 한 번 떴다가 크기가 바뀌는 것이
    // 보이지 않는데, 탭 복원은 첫 페인트 뒤라서 한쪽으로 몰 수가 없다. 파일은
    // 수 KB 짜리 JSON 하나다.
    restoreWindowGeometry( mrst::loadWorkspaceSession( lastRoot ).windowGeometry );
}

void MainWindow::applySessionLayout( const mrst::WorkspaceSession& session )
{
    if( !layoutRestoreEnabled() )
        return;

    // 도크 배치가 먼저다. restoreState() 가 편집기|프리뷰 스플리터를 담은 중앙
    // 도크를 재배치하므로, 순서를 뒤집으면 아래 setSizes() 가 그것에 덮인다.
    restoreDockLayout( session.dockLayout );

    const auto restoreSizes = [ this ]( QSplitter* splitter, const QList< int >& sizes ) {
        if( splitter == nullptr || sizes.size() != splitter->count() )
            return false;
        splitter->setSizes( sizes );
        return true;
    };
    previewSplitFromSession_ = restoreSizes( Ui.splitter_2, session.previewSplitterSizes );

    // 창 크기는 생성자가 이미 잡았으면 그대로 둔다. 여기까지 오는 사이에
    // 사용자가 창을 움직였을 수 있고, 그것을 되돌릴 이유가 없다. 남은 경로는
    // 명령줄 인자로 다른 워크스페이스를 연 경우다 — 그때는 생성자가 본 것과
    // 다른 세션이므로 지금 적용해야 한다.
    if( !windowGeometryRestored_ )
        restoreWindowGeometry( session.windowGeometry );
}

void MainWindow::restoreLastSession()
{
    const mrst::PhaseSpan restoreSpan( "session.restore" );
    const QString lastRoot = AppSettings().value( QStringLiteral( "workspace/lastRoot" ) ).toString();
    if( lastRoot.isEmpty() || mrst::isDisconnectedRemoteDrivePath( lastRoot )
        || !QFileInfo( lastRoot ).isDir() )
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

    // 배치는 탭을 다 만든 뒤에 적용해야 레이아웃이 다시 계산되며 덮이지 않는다.
    applySessionLayout( session );

    // activeIndex 는 **세션 문서 목록**의 번호다. 탭 위젯의 번호로 쓰면 안 된다.
    //
    // 두 목록은 어긋난다. 핫 엑시트 스냅샷이 우리보다 먼저 탭을 열어 두고
    // (restoreHotExitSnapshots 가 advanceStartupPhase 에서 앞선다), 그 사이 사라진
    // 파일은 위에서 건너뛴다. 밀린 자리를 그대로 고르면 엉뚱한 문서가 활성이 되고,
    // 그 자리가 이름 없는 버퍼면 경로가 없어 **프리뷰가 아예 만들어지지 않는다**
    // (requestPreviewBuild 의 `context->path.isEmpty()` 조기 반환).
    // 그래서 번호가 아니라 경로로 찾는다.
    if( QTextView* view = textViewForPath( mrst::activeDocumentPath( session ) ); view != nullptr )
    {
        if( const int tab = m_tabWidget->indexOf( view ); tab >= 0 )
            m_tabWidget->setCurrentIndex( tab );
    }

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
    //
    // 짝짓기는 탭 번호가 아니라 **경로**로 한다. 위와 같은 이유로 두 목록의 번호가
    // 어긋나서, 번호로 짝지으면 A 문서의 캐럿이 B 문서로 간다.
    for( const mrst::OpenDocumentState& state : session.documents )
    {
        QTextView* view = textViewForPath( state.path );
        if( view == nullptr )
            continue;   // 사라졌거나 열지 못한 문서

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

    // Markdown 탭을 연 뒤 늦게 도착한 이전 Sphinx 진단이 표를 다시 채우지 않게
    // 한다. 진단 저장소 자체는 지우지 않아 원래 reST 탭으로 돌아가면 복원된다.
    if( isMarkdownView( currentView() ) )
    {
        const QSignalBlocker blocker( table );
        table->setRowCount( 0 );
        return;
    }

    const QVector< mrst::DiagnosticEntry > entries = controller_->diagnostics()->all();

    // 세부에 행 수를 담는다. 이 함수가 **빌드당 몇 번** 불리는지가 3.1 의
    // 근거이므로, 트레이스에서 begin 줄을 세는 것 자체가 측정이다.
    const mrst::PhaseSpan span( "diag.table",
                               QStringLiteral( "rows=%1" ).arg( entries.size() ) );

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

    // 프리뷰 상태는 오른쪽 칩이 아니라 왼쪽의 진행 자리로 간다
    // (refreshStatusProgress). 오른쪽에는 가만히 있는 값만 남는다.
    envStatusLabel_ = new QPushButton( this );
    envStatusLabel_->setObjectName( QStringLiteral( "pythonEnvironmentStatus" ) );
    envStatusLabel_->setFlat( true );
    envStatusLabel_->setAutoDefault( false );
    envStatusLabel_->setDefault( false );
    envStatusLabel_->setContentsMargins( 8, 0, 8, 0 );
    envStatusLabel_->setAccessibleName( tr( "Python 환경 상태" ) );
    connect( envStatusLabel_, &QPushButton::clicked, this,
             &MainWindow::confirmRepairPythonEnvironment );
    statusBar()->addPermanentWidget( envStatusLabel_ );

    if( controller_ != nullptr )
    {
        connect( controller_, &mrst::WorkspaceController::previewStatusChanged, this,
                [this]( const QString& text, const bool busy, const int permille ) {
                    if( busy )
                    {
                        StatusTask task;
                        task.message  = text;
                        task.permille = permille;
                        // 프리뷰 빌드는 취소 단추를 내지 않는다. 취소해도 편집기는
                        // 그대로고, 다음 편집이 곧 새 빌드를 부르므로 사용자가
                        // 얻는 것이 없다.
                        statusTasks_.insert( StatusTaskId::Preview, task );
                    }
                    else
                        statusTasks_.remove( StatusTaskId::Preview );

                    refreshStatusProgress();
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
                 if( damagedPythonEnvironments_.isEmpty() && repairingPythonProjectKey_.isEmpty() )
                 {
                     envStatusLabel_->setText( percent < 0
                                                   ? tr( "환경: %1" ).arg( phase )
                                                   : tr( "환경: %1 (%2%)" ).arg( phase ).arg( percent ) );
                 }
             } );

    if( controller_ != nullptr )
    {
        connect( controller_, &mrst::WorkspaceController::pythonEnvironmentDamaged, this,
                [this]( const QString& projectKey, const QString& projectId,
                        const QString& environmentPath, const QString& reason ) {
                    damagedPythonEnvironments_.insert(
                        projectKey, DamagedPythonStatus{ projectId, environmentPath, reason } );
                    preferredDamagedPythonProjectKey_ = projectKey;
                    updateEnvStatusChip();
                } );
        connect( controller_, &mrst::WorkspaceController::pythonEnvironmentDamageCleared, this,
                [this]( const QString& projectKey ) {
                    damagedPythonEnvironments_.remove( projectKey );
                    if( preferredDamagedPythonProjectKey_ == projectKey )
                    {
                        preferredDamagedPythonProjectKey_ = damagedPythonEnvironments_.isEmpty()
                                                                ? QString{}
                                                                : damagedPythonEnvironments_.firstKey();
                    }
                    updateEnvStatusChip();
                } );
        connect( controller_, &mrst::WorkspaceController::pythonEnvironmentRepairStarted, this,
                [this]( const QString& projectKey ) {
                    repairingPythonProjectKey_ = projectKey;
                    pythonRepairPercent_ = 0;
                    pythonRepairPhase_ = tr( "교체 환경 준비 중" );
                    updateEnvStatusChip();
                } );
        connect( controller_, &mrst::WorkspaceController::pythonEnvironmentRepairProgress, this,
                [this]( const QString& projectKey, const int percent, const QString& phase ) {
                    if( repairingPythonProjectKey_ != projectKey )
                        return;
                    pythonRepairPercent_ = percent;
                    pythonRepairPhase_ = phase;
                    updateEnvStatusChip();
                } );
        connect( controller_, &mrst::WorkspaceController::pythonEnvironmentRepairFinished, this,
                [this]( const QString& projectKey, const bool success, const QString& message ) {
                    if( repairingPythonProjectKey_ == projectKey )
                        repairingPythonProjectKey_.clear();
                    pythonRepairPercent_ = -1;
                    pythonRepairPhase_.clear();
                    updateEnvStatusChip();
                    if( success )
                        showTransientStatus( tr( "프로젝트 Python 환경 복구 완료" ), 5000 );
                    else
                        QMessageBox::warning( this, tr( "Python 환경 복구 실패" ), message );
                } );
    }

    updateEnvStatusChip();

    // startup 을 막지 않는다. 창이 뜬 뒤에 백그라운드로 시작한다.
    if( pythonEnv_->autoBootstrap() )
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
            showTransientStatus( tr( "최신 버전을 사용하고 있습니다." ), 4000 );
    } );
    connect( updateService_, &mrst::UpdateService::failed, this,
            [this]( const QString& message, const bool silent ) {
                if( !silent )
                    QMessageBox::warning( this, tr( "업데이트" ), message );
            } );
    connect( updateService_, &mrst::UpdateService::installOutcomeReported, this,
            [this]( const bool succeeded, const QString& version, const QString& message ) {
                if( succeeded )
                    showTransientStatus( tr( "%1 로 업데이트했습니다." ).arg( version ), 8000 );
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

    envStatusLabel_->setAccessibleName( tr( "Python 환경 상태" ) );

    if( !repairingPythonProjectKey_.isEmpty() )
    {
        const QString phase = pythonRepairPhase_.isEmpty() ? tr( "복구 중" ) : pythonRepairPhase_;
        envStatusLabel_->setText( pythonRepairPercent_ < 0
                                      ? tr( "환경: %1" ).arg( phase )
                                      : tr( "환경: %1 (%2%)" ).arg( phase ).arg( pythonRepairPercent_ ) );
        envStatusLabel_->setIcon( style()->standardIcon( QStyle::SP_BrowserReload ) );
        envStatusLabel_->setToolTip( tr( "프로젝트 Python 환경을 복구하고 있습니다." ) );
        envStatusLabel_->setAccessibleDescription( envStatusLabel_->toolTip() );
        envStatusLabel_->setFocusPolicy( Qt::NoFocus );
        envStatusLabel_->setAttribute( Qt::WA_TransparentForMouseEvents, true );
        envStatusLabel_->unsetCursor();
        return;
    }

    if( !damagedPythonEnvironments_.isEmpty() )
    {
        const int count = damagedPythonEnvironments_.size();
        const auto it = damagedPythonEnvironments_.constFind( preferredDamagedPythonProjectKey_ );
        const DamagedPythonStatus damaged = it != damagedPythonEnvironments_.constEnd()
                                                   ? it.value()
                                                   : damagedPythonEnvironments_.constBegin().value();
        envStatusLabel_->setText( count == 1
                                      ? tr( "환경: 손상됨 · 클릭하여 복구" )
                                      : tr( "환경: %1개 손상됨 · 클릭하여 복구" ).arg( count ) );
        envStatusLabel_->setIcon( style()->standardIcon( QStyle::SP_MessageBoxWarning ) );
        envStatusLabel_->setToolTip(
            tr( "프로젝트: %1\n환경: %2\n원인: %3" )
                .arg( damaged.projectId,
                      QDir::toNativeSeparators( damaged.environmentPath ), damaged.reason ) );
        envStatusLabel_->setAccessibleDescription( envStatusLabel_->toolTip() );
        envStatusLabel_->setFocusPolicy( Qt::StrongFocus );
        envStatusLabel_->setAttribute( Qt::WA_TransparentForMouseEvents, false );
        envStatusLabel_->setCursor( Qt::PointingHandCursor );
        return;
    }

    envStatusLabel_->setIcon( {} );
    envStatusLabel_->setText( tr( "환경: %1" ).arg( pythonEnv_->stateText() ) );
    envStatusLabel_->setToolTip( pythonEnv_->isReady()
                                    ? QDir::toNativeSeparators( pythonEnv_->pythonExe() )
                                    : pythonEnv_->lastError() );
    envStatusLabel_->setAccessibleDescription( envStatusLabel_->toolTip() );
    envStatusLabel_->setFocusPolicy( Qt::NoFocus );
    envStatusLabel_->setAttribute( Qt::WA_TransparentForMouseEvents, true );
    envStatusLabel_->unsetCursor();
}

void MainWindow::confirmRepairPythonEnvironment()
{
    if( controller_ == nullptr || !repairingPythonProjectKey_.isEmpty()
        || damagedPythonEnvironments_.isEmpty() )
        return;

    auto it = damagedPythonEnvironments_.constFind( preferredDamagedPythonProjectKey_ );
    if( it == damagedPythonEnvironments_.constEnd() )
        it = damagedPythonEnvironments_.constBegin();

    const QString projectKey = it.key();
    const DamagedPythonStatus damaged = it.value();
    if( damaged.environmentPath.isEmpty() )
    {
        QMessageBox::warning(
            this, tr( "Python 환경 복구" ),
            tr( "자동 복구할 프로젝트 가상환경을 찾지 못했습니다.\n\n"
                "프로젝트: %1\n원인: %2\n\n"
                "환경 설정에서 지정한 Python 경로를 확인해 주세요." )
                .arg( damaged.projectId, damaged.reason ) );
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr( "Python 환경 복구" ),
        tr( "프로젝트 Python 환경이 손상되었습니다.\n\n"
            "프로젝트: %1\n환경: %2\n원인: %3\n\n"
            "pyproject.toml과 uv.lock을 사용해 같은 위치 옆에 새 환경을 구성하고, "
            "검증을 통과한 경우에만 교체합니다. 기존 손상 환경은 백업으로 남깁니다.\n\n"
            "지금 복구할까요?" )
            .arg( damaged.projectId,
                  QDir::toNativeSeparators( damaged.environmentPath ), damaged.reason ),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No );
    if( answer != QMessageBox::Yes )
        return;

    static_cast< void >( controller_->repairPythonEnvironment( projectKey ) );
}

void MainWindow::openStartupPaths( const QStringList& paths )
{
    if( paths.isEmpty() )
        return;

    if( mrst::isDisconnectedRemoteDrivePath( paths.first() ) )
        return;

    const QFileInfo first( paths.first() );
    if( !first.exists() )
        return;

    // 파일이 첫 인자면 상위 폴더를 워크스페이스로 삼아야 프로젝트 스캔이 동작한다.
    if( !setWorkspace( first.isDir() ? first.absoluteFilePath() : first.absolutePath() ) )
        return;

    // 세션 복원과 같은 이유로 활성 문서 반영을 마지막 한 번으로 접는다.
    if( controller_ )
        controller_->beginBatchRestore();

    for( const QString& path : paths )
    {
        if( mrst::isDisconnectedRemoteDrivePath( path ) )
            continue;

        const QFileInfo info( path );
        if( info.exists() && info.isFile() )
            openFile( info.absoluteFilePath() );
    }

    if( controller_ )
    {
        controller_->setActiveDocument( textViewOf( currentView() ) );
        controller_->endBatchRestore();
    }

    // 탭은 명령줄이 정했지만 화면 배치는 이 워크스페이스의 것이다. 지난번에
    // 이 폴더에서 잡아 둔 패널 크기로 열리는 편이, 인자를 주고 열었다는 이유만으로
    // 기본 배치로 돌아가는 것보다 낫다.
    applySessionLayout( mrst::loadWorkspaceSession( workspaceRoot_ ) );
}
