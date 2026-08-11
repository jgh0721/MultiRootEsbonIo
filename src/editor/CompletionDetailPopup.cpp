#include "stdafx.h"
#include "CompletionDetailPopup.hpp"

#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>

namespace mrst {

namespace {
/// 패널 고정 폭. 목록 팝업(280~720)과 나란히 놓아도 화면을 잡아먹지 않는 값.
constexpr int kPanelWidth = 340;
/// 본문이 길면 여기서 자른다. 정의 전문이 아니라 개요를 보여 주는 것이 목적이다.
constexpr int kMaxPanelHeight = 320;
/// 목록 팝업과의 간격.
constexpr int kAnchorGap = 6;
}  // namespace

CompletionDetailPopup::CompletionDetailPopup( QWidget* parent )
    : QFrame( parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint )
{
    setAttribute( Qt::WA_ShowWithoutActivating );
    setFocusPolicy( Qt::NoFocus );
    setFrameShape( QFrame::StyledPanel );
    setFrameShadow( QFrame::Plain );
    setAutoFillBackground( true );

    QPalette framePalette = palette();
    framePalette.setColor( QPalette::Window, framePalette.color( QPalette::Base ) );
    setPalette( framePalette );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 10, 8, 10, 8 );
    layout->setSpacing( 6 );

    titleLabel_ = new QLabel( this );
    titleLabel_->setWordWrap( true );
    titleLabel_->setTextInteractionFlags( Qt::NoTextInteraction );
    QFont titleFont = titleLabel_->font();
    titleFont.setBold( true );
    titleLabel_->setFont( titleFont );
    layout->addWidget( titleLabel_ );

    bodyLabel_ = new QLabel( this );
    bodyLabel_->setWordWrap( true );
    bodyLabel_->setAlignment( Qt::AlignTop | Qt::AlignLeft );
    bodyLabel_->setTextInteractionFlags( Qt::NoTextInteraction );
    layout->addWidget( bodyLabel_, 1 );

    sourceLabel_ = new QLabel( this );
    sourceLabel_->setWordWrap( true );
    sourceLabel_->setTextInteractionFlags( Qt::NoTextInteraction );
    QFont sourceFont = sourceLabel_->font();
    sourceFont.setPointSize( qMax( sourceFont.pointSize() - 1, 7 ) );
    sourceLabel_->setFont( sourceFont );
    QPalette dimmed = sourceLabel_->palette();
    QColor dimColor = dimmed.color( QPalette::Active, QPalette::WindowText );
    dimColor.setAlpha( 150 );
    dimmed.setColor( QPalette::All, QPalette::WindowText, dimColor );
    sourceLabel_->setPalette( dimmed );
    layout->addWidget( sourceLabel_ );
}

bool CompletionDetailPopup::setContent( const QString& title, const QString& body,
                                        const QString& source )
{
    const QString trimmedTitle = title.trimmed();
    const QString trimmedBody = body.trimmed();
    if( trimmedTitle.isEmpty() && trimmedBody.isEmpty() )
    {
        titleLabel_->clear();
        bodyLabel_->clear();
        sourceLabel_->clear();
        return false;
    }

    titleLabel_->setText( trimmedTitle );
    titleLabel_->setVisible( !trimmedTitle.isEmpty() );
    bodyLabel_->setText( trimmedBody );
    bodyLabel_->setVisible( !trimmedBody.isEmpty() );
    sourceLabel_->setText( source.trimmed() );
    sourceLabel_->setVisible( !source.trimmed().isEmpty() );

    resizeToContent();
    return true;
}

bool CompletionDetailPopup::hasContent() const
{
    return !titleLabel_->text().isEmpty() || !bodyLabel_->text().isEmpty();
}

void CompletionDetailPopup::resizeToContent()
{
    setFixedWidth( kPanelWidth );
    // heightForWidth 를 쓰려면 폭이 먼저 확정돼야 한다.
    const int hint = layout() != nullptr ? layout()->totalHeightForWidth( kPanelWidth ) : sizeHint().height();
    const int height = hint > 0 ? hint : sizeHint().height();
    setFixedHeight( qBound( 48, height, kMaxPanelHeight ) );
}

QPoint CompletionDetailPopup::clampToScreen( QPoint target, const QPoint& reference ) const
{
    const QScreen* screen = QGuiApplication::screenAt( reference );
    if( screen == nullptr )
        screen = QGuiApplication::primaryScreen();
    if( screen == nullptr )
        return target;

    const QRect available = screen->availableGeometry();
    target.setX( qBound( available.left(), target.x(), qMax( available.left(), available.right() - width() ) ) );
    target.setY( qBound( available.top(), target.y(), qMax( available.top(), available.bottom() - height() ) ) );
    return target;
}

void CompletionDetailPopup::showBesideAnchor( const QRect& anchor )
{
    if( !hasContent() )
    {
        hide();
        return;
    }

    const QScreen* screen = QGuiApplication::screenAt( anchor.topRight() );
    if( screen == nullptr )
        screen = QGuiApplication::primaryScreen();

    QPoint target( anchor.right() + kAnchorGap, anchor.top() );
    if( screen != nullptr && target.x() + width() > screen->availableGeometry().right() )
    {
        // 오른쪽이 좁으면 목록 왼쪽으로 뒤집는다.
        target.setX( anchor.left() - kAnchorGap - width() );
    }

    move( clampToScreen( target, anchor.topRight() ) );
    show();
    raise();
}

void CompletionDetailPopup::showNearPoint( const QPoint& globalPos )
{
    if( !hasContent() )
    {
        hide();
        return;
    }

    // 커서 바로 아래 오른쪽. 마우스 포인터가 패널을 가리지 않게 조금 띄운다.
    move( clampToScreen( globalPos + QPoint( 16, 20 ), globalPos ) );
    show();
    raise();
}

}  // namespace mrst
