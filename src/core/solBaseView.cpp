#include "stdafx.h"
#include "solBaseView.hpp"
#include "solQlementineTheme.hpp"
#include "solThemeManager.hpp"

#include <QFileInfo>
#include <QMetaObject>
#include <QVBoxLayout>

///////////////////////////////////////////////////////////////////////////////
///

QBaseView::QBaseView( QWidget* Parent )
    : QWidget( Parent )
{
    // WA_NoSystemBackground + WA_OpaquePaintEvent 는 paintEvent 로 모든 픽셀을 직접
    // 칠하는 위젯에만 쓸 수 있다. QBaseView 는 paintEvent 를 오버라이드하지 않으므로
    // 이 조합을 켜면 배경이 지워지지도, 그려지지도 않는다. 스플리터를 끌어 노출
    // 영역이 실시간으로 바뀔 때 이전 픽셀이 그대로 남아 검은 띠로 보였다.
    // (Scintilla 위젯 쪽의 같은 설정은 거기서 전면 페인트를 하므로 정당하다.)

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
    //
    // 부모는 도구모음 슬롯이다. 뷰를 부모로 삼으면 안 되는 이유는
    // setToolBarHost() 주석에 적어 두었다 (Qlementine 콤보박스 무한 재귀).
    // 슬롯이 없는 경우(단독 시험 등)에만 뷰로 떨어진다 — 부모를 아예 주지
    // 않으면 최상위 위젯이 되어 별도 창으로 떠 버린다.
    auto* tb = new QToolBar( m_toolBarHost ? m_toolBarHost.data() : static_cast< QWidget* >( this ) );
    tb->setMovable( false );
    tb->setIconSize( QSize( 16, 16 ) );
    tb->setToolButtonStyle( Qt::ToolButtonIconOnly );
    tb->setVisible( m_toolBarVisible );

    // 간격은 QStyle 의 pixelMetric 이 정한다. Qlementine 은 항목 4px / 여백 8px /
    // 구분선 16px 이라 항목이 넷뿐인 이 도구모음에서는 지나치게 성기게 보인다.
    // 레이아웃에 직접 값을 주면 스타일 값을 덮어쓴다.
    //
    // 여백은 **사방을 같은 값으로** 줘야 한다. QToolBarLayout 은 배치할 때
    // margin() 하나(왼쪽 값)만 읽어 위아래에도 그대로 쓰는데, 높이는
    // QLayout::totalSizeHint 가 실제 위/아래 여백으로 계산한다. 좌우와 상하를
    // 다르게 주면 항목이 왼쪽 여백만큼 아래로 밀린 채 도구모음 높이는 그대로라
    // 콤보박스 아랫부분이 잘린다. 4px 는 Qlementine 의 포커스 테두리(항목
    // 위아래로 4px 씩 번진다)가 잘리지 않는 최소값이다.
    if( QLayout* layout = tb->layout() )
    {
        layout->setSpacing( 2 );
        layout->setContentsMargins( 4, 4, 4, 4 );
    }
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

    // Qlementine QStyle 이 켜져 있으면 뷰 안쪽도 QStyle 이 그린다.
    // 아래 스타일시트를 씌우면 자식 위젯까지 전부 덮어써 버린다.
    if( QlementineTheme::isActive() )
    {
        setStyleSheet( QString() );
        return;
    }

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
