#include "stdafx.h"
#include "solBaseView.hpp"
#include "solThemeManager.hpp"

#include <QFileInfo>
#include <QMetaObject>
#include <QVBoxLayout>

///////////////////////////////////////////////////////////////////////////////
///

QBaseView::QBaseView( QWidget* Parent )
    : QWidget( Parent )
{
    setAttribute( Qt::WA_NoSystemBackground );
    setAttribute( Qt::WA_OpaquePaintEvent );

    auto* lay = new QVBoxLayout( this );
    lay->setContentsMargins( 0, 0, 0, 0 );
    lay->setSpacing( 0 );
}

QBaseView::~QBaseView()
{
    m_shuttingDown = true;
}

QString QBaseView::title() const
{
    if( !m_displayTitle.isEmpty() )
        return m_displayTitle;

    if( m_filePath.isEmpty() )
        return tr( "제목없음" );
    return QFileInfo( m_filePath ).fileName();
}

///////////////////////////////////////////////////////////////////////////////
// 뷰 독립 도구모음

QToolBar* QBaseView::createToolBar()
{
    // MainWindow에서 탭 전환 시 이전 도구모음은 삭제되므로
    // 매번 새로 생성해야 함 (m_toolBar 캐싱하지 않음)
    auto* tb = new QToolBar( this );
    tb->setMovable( false );
    tb->setIconSize( QSize( 20, 20 ) );
    tb->setToolButtonStyle( Qt::ToolButtonIconOnly );
    tb->setVisible( m_toolBarVisible );
    m_toolBar = tb;
    connect( tb, &QObject::destroyed, this, [this, tb] {
        if( m_toolBar == tb ) m_toolBar = nullptr;
    } );
    return tb;
}

void QBaseView::setToolBarVisible( bool visible )
{
    m_toolBarVisible = visible;
    if( m_toolBar )
        m_toolBar->setVisible( visible );
    if( m_auxiliaryToolBar )
        m_auxiliaryToolBar->setVisible( visible );
}

///////////////////////////////////////////////////////////////////////////////
// 테마

void QBaseView::setTheme( Theme theme )
{
    m_theme = theme;
    applyThemeStyleSheet( theme );
}

///////////////////////////////////////////////////////////////////////////////
///

void QBaseView::setDisplayTitle( const QString& title )
{
    m_displayTitle = title;
}

void QBaseView::applyThemeStyleSheet( Theme theme )
{
    Q_UNUSED( theme );
    auto& tm = ThemeManager::instance();
    setStyleSheet( QStringLiteral(
        "QWidget { background-color: %1; color: %2; }"
        "QToolBar { background-color: %3; border: none; }"
        "QTabBar::tab { background: %5; color: %2; padding: 6px 12px; }"
        "QTabBar::tab:selected { background: %4; }"
    ).arg( tm.backgroundColor().name(),
           tm.foregroundColor().name(),
           tm.toolBarColor().name(),
           tm.tabActiveColor().name(),
           tm.tabInactiveColor().name() ) );
}

void QBaseView::updateCopyAvailability()
{
    const bool available = canCopyToClipboard();
    if( m_lastCopyAvailability == available )
        return;

    m_lastCopyAvailability = available;
    emit sigCopyAvailabilityChanged( available );
}

void QBaseView::beginLoading( const QString& message, int maximum )
{
    m_loadingActive = true;
    m_loadingMessage = message;
    m_loadingValue = 0;
    m_loadingMaximum = qMax( 0, maximum );
    emit sigLoadingStateChanged( true, m_loadingMessage, m_loadingValue, m_loadingMaximum );
}

void QBaseView::updateLoadingProgress( const QString& message, int value, int maximum, QEventLoop::ProcessEventsFlags flags )
{
    Q_UNUSED( flags );

    if( !m_loadingActive )
        m_loadingActive = true;

    if( !message.isNull() )
        m_loadingMessage = message;
    if( maximum >= 0 )
        m_loadingMaximum = maximum;
    if( value >= 0 )
        m_loadingValue = value;

    emit sigLoadingStateChanged( true, m_loadingMessage, m_loadingValue, m_loadingMaximum );
}

void QBaseView::endLoading()
{
    if( !m_loadingActive && m_loadingMessage.isEmpty() && m_loadingMaximum == 0 )
        return;

    m_loadingActive = false;
    m_loadingMessage.clear();
    m_loadingValue = 0;
    m_loadingMaximum = 0;
    emit sigLoadingStateChanged( false, {}, 0, 0 );
}
