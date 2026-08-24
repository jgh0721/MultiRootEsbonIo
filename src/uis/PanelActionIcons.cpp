#include "stdafx.h"
#include "uis/PanelActionIcons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <functional>

namespace mrst::panelicons {

namespace {

/// 도안을 그리는 좌표계. 16 칸 격자에서 그리고 실제 크기로 늘린다.
constexpr qreal kGrid = 16.0;

/// 아이콘 하나가 담는 크기들.
///
/// 한 크기만 넣으면 Qt 가 그것을 늘리거나 줄여 흐려진다. 도구 단추는 보통
/// 16 이지만 고DPI 화면과 큰 아이콘 설정에서 24·32 를 요구한다.
///
/// 48 은 **화면 배율 때문에** 있다. QIcon::pixmap(24) 는 배율 1.5 화면에서 36
/// 물리 픽셀을 청하는데, 그보다 큰 그림이 없으면 Qt 는 32 를 그대로 주고 배율을
/// 32/36 만큼 되계산한다 — 위젯이 그것을 다시 늘리므로 흐려지고, 되계산한 배율이
/// 반올림되어 논리 크기도 24 가 아니게 된다(24.06). 32 위에 한 단계를 더 두면
/// 배율 1.25·1.5·2 에서 모두 정확히 축소되어 두 문제가 함께 사라진다.
constexpr int kSizes[] = { 16, 20, 24, 32, 48 };

using Draw = std::function< void( QPainter&, const QColor& ) >;

QPixmap render( const Draw& draw, const QColor& color, const int size )
{
    QPixmap pixmap( size, size );
    pixmap.fill( Qt::transparent );

    QPainter painter( &pixmap );
    painter.setRenderHint( QPainter::Antialiasing, true );
    // 도안은 전부 선이다. 기본 브러시가 NoBrush 이긴 하지만, 채워진 도형이
    // 하나 섞이면 다른 것들까지 검게 나오므로 여기서 못 박는다.
    painter.setBrush( Qt::NoBrush );
    painter.scale( size / kGrid, size / kGrid );
    draw( painter, color );
    painter.end();
    return pixmap;
}

/// 팔레트의 두 그룹으로 Normal / Disabled 를 각각 만든다.
///
/// Qt 가 Normal 하나로 Disabled 를 유추해 주기는 하지만, 그 결과는 회색으로
/// 눌러 칠한 것이라 다크 테마에서 배경과 붙어 사라진다. 테마가 이미 알고 있는
/// 색(Disabled/ButtonText)을 쓰는 편이 두 테마 모두에서 맞다.
QIcon build( const Draw& draw, const QPalette& palette )
{
    const QColor normal = palette.color( QPalette::Active, QPalette::ButtonText );
    const QColor disabled = palette.color( QPalette::Disabled, QPalette::ButtonText );

    QIcon icon;
    for( const int size : kSizes )
    {
        icon.addPixmap( render( draw, normal, size ), QIcon::Normal );
        icon.addPixmap( render( draw, disabled, size ), QIcon::Disabled );
    }
    return icon;
}

QPen strokePen( const QColor& color, const qreal width = 1.3 )
{
    QPen pen( color );
    pen.setWidthF( width );
    pen.setCapStyle( Qt::RoundCap );
    pen.setJoinStyle( Qt::RoundJoin );
    return pen;
}

/// 오른쪽 아래에 붙는 작은 `+`. "새로 만들기" 둘이 공유한다.
void drawPlus( QPainter& painter, const QColor& color )
{
    painter.setPen( strokePen( color, 1.6 ) );
    painter.drawLine( QPointF( 12.4, 10.0 ), QPointF( 12.4, 14.4 ) );
    painter.drawLine( QPointF( 10.2, 12.2 ), QPointF( 14.6, 12.2 ) );
}

void drawNewFile( QPainter& painter, const QColor& color )
{
    painter.setPen( strokePen( color ) );

    // 모서리가 접힌 문서. 오른쪽 아래는 `+` 자리라 열어 둔다.
    QPainterPath page;
    page.moveTo( 2.6, 1.4 );
    page.lineTo( 7.0, 1.4 );
    page.lineTo( 10.0, 4.4 );
    page.lineTo( 10.0, 10.2 );
    painter.drawPath( page );

    QPainterPath body;
    body.moveTo( 2.6, 1.4 );
    body.lineTo( 2.6, 14.2 );
    body.lineTo( 8.4, 14.2 );
    painter.drawPath( body );

    painter.drawPolyline( QPolygonF{ { 7.0, 1.4 }, { 7.0, 4.4 }, { 10.0, 4.4 } } );

    drawPlus( painter, color );
}

void drawNewFolder( QPainter& painter, const QColor& color )
{
    painter.setPen( strokePen( color ) );

    // 탭이 달린 폴더. 오른쪽 아래는 `+` 자리라 열어 둔다.
    QPainterPath folder;
    folder.moveTo( 9.6, 13.4 );
    folder.lineTo( 1.6, 13.4 );
    folder.lineTo( 1.6, 3.2 );
    folder.lineTo( 5.6, 3.2 );
    folder.lineTo( 7.1, 5.2 );
    folder.lineTo( 13.4, 5.2 );
    folder.lineTo( 13.4, 8.8 );
    painter.drawPath( folder );

    drawPlus( painter, color );
}

void drawRename( QPainter& painter, const QColor& color )
{
    painter.setPen( strokePen( color ) );

    // 대각선으로 누운 연필. 몸통 · 촉 · 지우개 경계.
    painter.drawPolygon( QPolygonF{
        { 10.4, 1.9 }, { 13.6, 5.1 }, { 5.2, 13.5 }, { 1.9, 14.1 }, { 2.5, 10.8 } } );
    painter.drawLine( QPointF( 8.6, 3.7 ), QPointF( 11.8, 6.9 ) );
    painter.drawLine( QPointF( 2.5, 10.8 ), QPointF( 5.2, 13.5 ) );
}

void drawRemove( QPainter& painter, const QColor& color )
{
    painter.setPen( strokePen( color ) );

    // 뚜껑과 손잡이.
    painter.drawLine( QPointF( 2.2, 4.2 ), QPointF( 13.8, 4.2 ) );
    painter.drawPolyline( QPolygonF{ { 6.2, 4.2 }, { 6.2, 2.2 }, { 9.8, 2.2 }, { 9.8, 4.2 } } );

    // 몸통.
    painter.drawPolyline(
        QPolygonF{ { 3.6, 4.2 }, { 4.3, 14.0 }, { 11.7, 14.0 }, { 12.4, 4.2 } } );

    // 안쪽 홈 둘.
    painter.drawLine( QPointF( 6.7, 6.6 ), QPointF( 6.9, 11.8 ) );
    painter.drawLine( QPointF( 9.3, 6.6 ), QPointF( 9.1, 11.8 ) );
}

void drawFilter( QPainter& painter, const QColor& color )
{
    painter.setPen( strokePen( color ) );
    painter.drawEllipse( QPointF( 7.0, 7.0 ), 4.4, 4.4 );
    painter.drawLine( QPointF( 10.3, 10.3 ), QPointF( 14.2, 14.2 ) );
}

/// 색 견본 한 장. `scale` 은 화면 배율이다 (물리 픽셀 = 논리 크기 × scale).
QPixmap renderSwatch( const QColor& color, const QSize& size, const QPalette& palette, const qreal scale )
{
    const int width  = qMax( 1, qRound( size.width() * scale ) );
    const int height = qMax( 1, qRound( size.height() * scale ) );

    QPixmap pixmap( width, height );
    pixmap.fill( Qt::transparent );

    QPainter painter( &pixmap );

    // 반투명한 색과 불투명한 색은 체커보드가 없으면 화면에서 구분할 수 없다.
    // 알파가 온전한 색에는 깔지 않는다 — 어차피 덮이는데 칸마다 그리는 값이다.
    if( color.alpha() < 255 )
    {
        const int    cell = qMax( 2, qRound( 4.0 * scale ) );
        const QColor even = palette.color( QPalette::Active, QPalette::Base );
        // AlternateBase 는 테마에 따라 Base 와 같을 수 있다. 그러면 체커보드가
        // 단색이 되어 아무 구실도 못 하므로 밝기에서 직접 만든다.
        const QColor odd = even.lightnessF() < 0.5 ? even.lighter( 145 ) : even.darker( 112 );
        for( int y = 0; y < height; y += cell )
        {
            for( int x = 0; x < width; x += cell )
                painter.fillRect( QRect( x, y, cell, cell ),
                                  ( ( x / cell + y / cell ) % 2 == 0 ) ? even : odd );
        }
    }

    painter.fillRect( QRect( 0, 0, width, height ), color );

    // 테두리는 장식이 아니다. 없으면 표 배경과 같은 색인 견본이 빈 칸처럼 보인다.
    QColor borderColor = palette.color( QPalette::Active, QPalette::Text );
    borderColor.setAlpha( 110 );
    const qreal penWidth = qMax( 1.0, scale );
    QPen        pen( borderColor );
    pen.setWidthF( penWidth );
    painter.setPen( pen );
    painter.setBrush( Qt::NoBrush );
    // 선의 가운데가 그림 밖으로 반쯤 걸치지 않게 반 폭만큼 안으로 들인다.
    const qreal inset = penWidth / 2.0;
    painter.drawRect( QRectF( inset, inset, width - penWidth, height - penWidth ) );

    painter.end();
    return pixmap;
}

}  // namespace

QIcon newFile( const QPalette& palette )
{
    return build( drawNewFile, palette );
}

QIcon newFolder( const QPalette& palette )
{
    return build( drawNewFolder, palette );
}

QIcon rename( const QPalette& palette )
{
    return build( drawRename, palette );
}

QIcon remove( const QPalette& palette )
{
    return build( drawRemove, palette );
}

QIcon filter( const QPalette& palette )
{
    return build( drawFilter, palette );
}

QIcon colorSwatch( const QColor& color, const QSize& size, const QPalette& palette )
{
    if( !color.isValid() || size.isEmpty() )
        return {};

    QIcon icon;
    // 배율마다 딱 맞는 그림을 담는다. 한 장만 넣으면 Qt 가 그것을 늘리거나 줄여
    // 테두리가 흐려진다 — 위 kSizes 주석과 같은 이유다. 다만 여기서는 논리 크기가
    // 호출 측이 정하는 값이라 절대 크기 대신 배율로 담는다.
    for( const qreal scale : { 1.0, 1.25, 1.5, 2.0, 3.0 } )
        icon.addPixmap( renderSwatch( color, size, palette, scale ) );
    return icon;
}

}  // namespace mrst::panelicons
