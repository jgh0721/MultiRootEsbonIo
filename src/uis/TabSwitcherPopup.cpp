#include "stdafx.h"
#include "TabSwitcherPopup.hpp"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QScreen>
#include <QVBoxLayout>

namespace mrst {
namespace {

constexpr int kMaxVisibleRows = 14;
constexpr int kMinWidth = 320;
constexpr int kMaxWidth = 720;
constexpr int kFrameMargin = 8;
constexpr int kRolePath = Qt::UserRole + 1;
constexpr int kRoleTabIndex = Qt::UserRole + 2;

}  // namespace

TabSwitcherPopup::TabSwitcherPopup( QWidget* parent )
    : QFrame( parent, Qt::Popup )
{
    setFrameShape( QFrame::StyledPanel );
    setFrameShadow( QFrame::Raised );
    setAutoFillBackground( true );

    QPalette framePalette = palette();
    framePalette.setColor( QPalette::Window, framePalette.color( QPalette::Base ) );
    setPalette( framePalette );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( kFrameMargin, 6, kFrameMargin, 6 );
    layout->setSpacing( 4 );

    //: Ctrl+Tab 팝업의 제목. Visual Studio 의 "Active Files" 자리다.
    auto* header = new QLabel( tr( "열린 문서" ), this );
    QFont headerFont = header->font();
    headerFont.setBold( true );
    header->setFont( headerFont );
    layout->addWidget( header );

    list_ = new QListWidget( this );
    // 안쪽 목록이 포커스를 가지면 Tab 과 Ctrl 뗌을 이 위젯이 못 받는다
    // (헤더의 주석 참고).
    list_->setFocusPolicy( Qt::NoFocus );
    list_->setFrameShape( QFrame::NoFrame );
    list_->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    list_->setSelectionMode( QAbstractItemView::SingleSelection );
    list_->setUniformItemSizes( true );

    // 팝업 창은 활성화되지 않으므로 강조색이 Inactive 그룹에서 나온다. 그것을
    // Active 값으로 덮지 않으면 "무엇이 선택돼 있는지" 가 거의 보이지 않는다.
    QPalette listPalette = list_->palette();
    listPalette.setColor( QPalette::Inactive, QPalette::Highlight,
                         listPalette.color( QPalette::Active, QPalette::Highlight ) );
    listPalette.setColor( QPalette::Inactive, QPalette::HighlightedText,
                         listPalette.color( QPalette::Active, QPalette::HighlightedText ) );
    list_->setPalette( listPalette );
    layout->addWidget( list_ );

    detailLabel_ = new QLabel( this );
    detailLabel_->setTextFormat( Qt::PlainText );
    QPalette detailPalette = detailLabel_->palette();
    detailPalette.setColor( QPalette::WindowText, detailPalette.color( QPalette::PlaceholderText ) );
    detailLabel_->setPalette( detailPalette );
    layout->addWidget( detailLabel_ );

    connect( list_, &QListWidget::clicked, this, [this]( const QModelIndex& ) { chooseCurrent(); } );
    connect( list_, &QListWidget::currentRowChanged, this, [this]( int ) { refreshDetail(); } );
}

void TabSwitcherPopup::showEntries( const QList< TabSwitcherEntry >& entries, const int startRow )
{
    if( entries.isEmpty() )
        return;

    list_->clear();
    for( const TabSwitcherEntry& entry : entries )
    {
        auto* item = new QListWidgetItem( entry.icon, entry.title, list_ );
        item->setData( kRolePath, entry.detail );
        item->setData( kRoleTabIndex, entry.tabIndex );
    }

    // 폭은 가장 긴 제목과 경로에 맞춘다. 목록이 잘려 "..." 만 보이면 어느
    // 파일인지 구분하려고 열은 팝업이 제 일을 못 한다.
    const QFontMetrics listMetrics( list_->font() );
    const QFontMetrics detailMetrics( detailLabel_->font() );
    int contentWidth = 0;
    for( const TabSwitcherEntry& entry : entries )
    {
        contentWidth = qMax( contentWidth, listMetrics.horizontalAdvance( entry.title ) );
        contentWidth = qMax( contentWidth, detailMetrics.horizontalAdvance( entry.detail ) );
    }
    // 아이콘 자리 + 항목 여백 + 세로 스크롤바 + 레이아웃 여백.
    contentWidth += 64;

    const int rowHeight = list_->sizeHintForRow( 0 ) > 0 ? list_->sizeHintForRow( 0 )
                                                         : listMetrics.height() + 6;
    const int visibleRows = qMin( entries.size(), kMaxVisibleRows );
    list_->setFixedHeight( rowHeight * visibleRows + 2 );

    setFixedWidth( qBound( kMinWidth, contentWidth, kMaxWidth ) );
    adjustSize();

    const int row = ( startRow >= 0 && startRow < entries.size() ) ? startRow : 0;
    list_->setCurrentRow( row );
    refreshDetail();

    // 부모 창 가운데. 부모가 없거나 화면 밖으로 나가면 화면 가운데로 물러선다.
    const QWidget* anchor = parentWidget() != nullptr ? parentWidget()->window() : nullptr;
    const QRect reference = anchor != nullptr
                                ? QRect( anchor->mapToGlobal( QPoint( 0, 0 ) ), anchor->size() )
                                : ( screen() != nullptr ? screen()->availableGeometry() : QRect() );
    if( !reference.isEmpty() )
        move( reference.center() - QPoint( width() / 2, height() / 2 ) );

    show();
    raise();
}

void TabSwitcherPopup::step( const int delta )
{
    const int count = list_->count();
    if( count <= 0 )
        return;

    const int current = qMax( 0, list_->currentRow() );
    list_->setCurrentRow( ( ( current + delta ) % count + count ) % count );
}

void TabSwitcherPopup::refreshDetail()
{
    const QListWidgetItem* item = list_->currentItem();
    const QString path = item != nullptr ? item->data( kRolePath ).toString() : QString{};
    if( path.isEmpty() )
    {
        //: Ctrl+Tab 팝업 아래쪽. 이름 없는 버퍼라 보여 줄 경로가 없다는 뜻이다.
        detailLabel_->setText( tr( "저장되지 않은 문서" ) );
        return;
    }

    // 가운데를 줄인다. QLabel 은 넘치는 글자를 그냥 자르는데, 경로는 **끝**에
    // 파일 이름이 있어서 뒤가 잘리면 어느 파일인지 알 수 없게 된다.
    const QFontMetrics metrics( detailLabel_->font() );
    const int available = qMax( 40, width() - 2 * kFrameMargin );
    detailLabel_->setText( metrics.elidedText( path, Qt::ElideMiddle, available ) );
}

void TabSwitcherPopup::chooseCurrent()
{
    const QListWidgetItem* item = list_->currentItem();
    if( item == nullptr )
    {
        close();
        return;
    }

    const int tabIndex = item->data( kRoleTabIndex ).toInt();
    // 먼저 닫는다. 탭 전환은 프리뷰 빌드까지 끌고 오므로, 키보드 그랩을 쥔
    // 채로 들어가면 그동안 입력이 이 팝업에 갇힌다.
    close();
    emit tabChosen( tabIndex );
}

void TabSwitcherPopup::keyPressEvent( QKeyEvent* event )
{
    switch( event->key() )
    {
        case Qt::Key_Tab:
            // Shift 를 함께 누르면 보통 Key_Backtab 으로 오지만, 그렇지 않은
            // 조합/플랫폼도 있어 양쪽을 다 본다.
            step( ( event->modifiers() & Qt::ShiftModifier ) != 0 ? -1 : 1 );
            return;
        case Qt::Key_Backtab:
        case Qt::Key_Up:
        case Qt::Key_Left:
            step( -1 );
            return;
        case Qt::Key_Down:
        case Qt::Key_Right:
            step( 1 );
            return;
        case Qt::Key_Home:
            list_->setCurrentRow( 0 );
            return;
        case Qt::Key_End:
            list_->setCurrentRow( list_->count() - 1 );
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            chooseCurrent();
            return;
        case Qt::Key_Escape:
            close();
            return;
        default:
            break;
    }

    QFrame::keyPressEvent( event );
}

void TabSwitcherPopup::keyReleaseEvent( QKeyEvent* event )
{
    // Ctrl 을 떼면 확정이다. 이 팝업은 Ctrl 이 눌린 채로 열리므로, 이 한 줄이
    // 목록을 닫는 주 경로다.
    if( event->key() == Qt::Key_Control )
    {
        chooseCurrent();
        return;
    }

    QFrame::keyReleaseEvent( event );
}

}  // namespace mrst
