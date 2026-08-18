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
    const QRect available = screen != nullptr ? screen->availableGeometry() : QRect{};

    // 오른쪽 → 왼쪽 → 아래 → 위. 앞의 둘만 보고 실패하면 clampToScreen 이
    // 화면 안으로 밀어 넣는데, 목록은 720px 까지 넓어질 수 있어서 그때
    // 상세 패널이 목록 위에 겹친다 — 둘 다 못 읽게 된다.
    const QPoint candidates[] = {
        { anchor.right() + kAnchorGap, anchor.top() },
        { anchor.left() - kAnchorGap - width(), anchor.top() },
        { anchor.left(), anchor.bottom() + kAnchorGap },
        { anchor.left(), anchor.top() - kAnchorGap - height() },
    };

    QPoint target = clampToScreen( candidates[ 0 ], anchor.topRight() );
    for( const QPoint& candidate : candidates )
    {
        const QRect rect( candidate, size() );
        if( !available.isNull() && !available.contains( rect ) )
            continue;
        if( rect.intersects( anchor ) )
            continue;
        target = candidate;
        break;
    }

    // 어디에도 온전히 들어가지 않았다. 밀어 넣되 목록을 덮지 않는 쪽을 고른다.
    if( QRect( target, size() ).intersects( anchor ) )
    {
        for( const QPoint& candidate : candidates )
        {
            const QPoint clamped = clampToScreen( candidate, anchor.center() );
            if( !QRect( clamped, size() ).intersects( anchor ) )
            {
                target = clamped;
                break;
            }
        }
    }

    // 위치가 그대로면 건드리지 않는다. Windows 에서 move+show+raise 를 반복하면
    // 방향키를 훑을 때마다 패널이 깜빡인다.
    if( isVisible() && pos() == target )
        return;

    move( target );
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
