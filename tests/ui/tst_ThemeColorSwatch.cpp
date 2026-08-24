#include "TestRunner.hpp"

#include "uis/PanelActionIcons.hpp"

#include <oclero/qlementine/style/QlementineStyle.hpp>

#include <QHeaderView>
#include <QImage>
#include <QSet>
#include <QTableWidget>
#include <QTest>

#include <memory>

/// 설정 → 테마의 색상 칸이 **행마다 다른 색**으로 보이는지 지킨다.
///
/// 원래는 `QTableWidgetItem::setBackground()` 로 색을 실어 보냈다. 그런데
/// Qlementine 은 `PE_PanelItemViewItem` 에서 셀 배경을 자기가 칠하면서
/// `QStyleOptionViewItem::backgroundBrush` 를 읽지 않고, 색을 정하는
/// `listItemBackgroundColor()` 는 모델 인덱스를 아예 무시한다(`Q_UNUSED(index)`).
/// 그래서 **표 전체가 같은 색**이 되었다. 화면으로는 "색이 안 나온다" 로만 보이고
/// 어느 경로가 색을 버렸는지는 드러나지 않는다.
///
/// 그림을 직접 검사하지 않으면 이 부류는 잡히지 않는다. 항목에 브러시를 넣는
/// 코드는 그대로 성공하고 스타일이 조용히 버릴 뿐이므로, 값을 확인하는 테스트는
/// 통과한다. 그래서 여기서는 진짜 위젯을 Qlementine 스타일로 그려 **픽셀을**
/// 견준다 — `QWidget::grab()` 은 컴포지터를 거치지 않아 색이 결정적이다.
///
/// `dlgSettings.cpp` 는 링크하지 않는다(딸려 오는 것이 너무 많다). 검사 대상은
/// 색을 나르는 통로이므로 견본 아이콘과 표만으로 충분하다.
class ThemeColorSwatchTest : public QObject
{
    Q_OBJECT

private slots:
    void swatchPixmapCarriesTheColor();
    void differentColorsLookDifferentInTheTable();
    void translucentColorShowsTheCheckerboard();

private:
    /// 그림에서 그 색이 차지하는 세로 범위.
    struct Extent
    {
        bool found  = false;
        int  top    = 0;
        int  bottom = 0;
    };

    /// 열 하나에 견본 아이콘만 담은 표. 실제 설정 대화상자의 색상 열과 같은 구성이다.
    [[nodiscard]] static QTableWidget* makeTable( oclero::qlementine::QlementineStyle* style,
                                                 const QList< QColor >& colors, QSize swatchSize );
    /// 그 색이 찍힌 픽셀들의 세로 범위.
    ///
    /// **위젯 좌표를 쓰지 않는다.** 아이콘의 여백·세로 정렬·행 높이는 스타일이
    /// 정하고 창이 얼마나 드러났는지에 따라 달라져서, 좌표로 자리를 집으면 그리기와
    /// 무관한 이유로 테스트가 깨진다. 색이 그림 어디에 찍혔는지만 보고 행 순서를
    /// 되짚는다 — 견본은 행 순서대로 그려지므로 그것으로 충분하다.
    [[nodiscard]] static Extent extentOf( const QImage& image, const QColor& color );
};

QTableWidget* ThemeColorSwatchTest::makeTable( oclero::qlementine::QlementineStyle* style,
                                               const QList< QColor >& colors, const QSize swatchSize )
{
    auto* table = new QTableWidget( colors.size(), 1 );
    // 표에 직접 씌운다. 항목 델리게이트는 option.widget->style() 을 쓰므로
    // 이 한 줄이 셀 그리기 전체를 Qlementine 으로 보낸다.
    table->setStyle( style );
    table->setIconSize( swatchSize );
    table->horizontalHeader()->setVisible( false );
    table->verticalHeader()->setVisible( false );
    table->setColumnWidth( 0, swatchSize.width() + 24 );
    // 선택 표시가 셀 배경을 덮는다. 색을 견주는 자리라 선택을 없애 둔다.
    table->setSelectionMode( QAbstractItemView::NoSelection );

    for( int row = 0; row < colors.size(); ++row )
    {
        auto* item = new QTableWidgetItem;
        item->setIcon( mrst::panelicons::colorSwatch( colors.at( row ), swatchSize, table->palette() ) );
        table->setItem( row, 0, item );
    }

    // 행 높이를 못 박고 그만큼 창을 준다. 스타일이 정하는 높이에 맡기면 마지막
    // 행이 뷰포트 밖으로 밀려 그림에 담기지 않는다.
    table->verticalHeader()->setDefaultSectionSize( swatchSize.height() + 14 );
    table->setShowGrid( false );
    table->resize( swatchSize.width() + 80,
                   ( swatchSize.height() + 14 ) * colors.size() + 8 );
    return table;
}

ThemeColorSwatchTest::Extent ThemeColorSwatchTest::extentOf( const QImage& image, const QColor& color )
{
    const QRgb wanted = color.rgb() | 0xff000000u;

    Extent extent;
    for( int y = 0; y < image.height(); ++y )
    {
        for( int x = 0; x < image.width(); ++x )
        {
            if( ( image.pixel( x, y ) | 0xff000000u ) != wanted )
                continue;
            if( !extent.found )
            {
                extent.found = true;
                extent.top   = y;
            }
            extent.bottom = y;
            break;
        }
    }
    return extent;
}

void ThemeColorSwatchTest::swatchPixmapCarriesTheColor()
{
    const QSize size( 28, 14 );
    const QIcon icon = mrst::panelicons::colorSwatch( QColor( QStringLiteral( "#ff3366" ) ), size, QPalette() );
    QVERIFY( !icon.isNull() );

    const QImage image = icon.pixmap( size ).toImage();
    QVERIFY( !image.isNull() );
    // 테두리를 피해 가운데를 본다.
    QCOMPARE( QColor( image.pixel( image.width() / 2, image.height() / 2 ) ),
              QColor( QStringLiteral( "#ff3366" ) ) );
}

void ThemeColorSwatchTest::differentColorsLookDifferentInTheTable()
{
    auto style = std::make_unique< oclero::qlementine::QlementineStyle >();

    const QColor first( QStringLiteral( "#ff0000" ) );
    const QColor second( QStringLiteral( "#00c000" ) );
    const QColor third( QStringLiteral( "#2060ff" ) );

    const QList< QColor > colors{ first, second, third };

    std::unique_ptr< QTableWidget > table( makeTable( style.get(), colors, QSize( 28, 14 ) ) );
    table->show();
    QVERIFY( QTest::qWaitForWindowExposed( table.get() ) );
    // 창이 드러난 것과 항목 뷰가 자리를 다 잡은 것은 다르다. 한 박자 기다리지
    // 않으면 아직 안 그려진 행을 담는 일이 있다.
    QTest::qWait( 150 );

    const QImage shot = table->grab().toImage();
    QVERIFY( !shot.isNull() );

    QList< Extent > extents;
    for( int row = 0; row < colors.size(); ++row )
    {
        const Extent extent = extentOf( shot, colors.at( row ) );
        // 예전 구현(setBackground)에서는 Qlementine 이 브러시를 버려서 이 색이
        // 그림 어디에도 없었다 — 그것이 이 단언이 막으려는 상태다.
        QVERIFY2( extent.found,
                  qPrintable( QStringLiteral( "%1행의 색 %2 가 그림에 없다" )
                                      .arg( row )
                                      .arg( colors.at( row ).name() ) ) );
        extents.append( extent );
    }

    // 색마다 자기 행에만 있어야 한다. 세로 범위가 겹치지 않고 행 순서대로
    // 놓였다는 것이 곧 "칸마다 제 색" 이라는 뜻이다. 모든 칸이 같은 색이던
    // 원래 증상이라면 아래가 무너진다.
    for( int row = 1; row < extents.size(); ++row )
    {
        QVERIFY2( extents.at( row - 1 ).bottom < extents.at( row ).top,
                  qPrintable( QStringLiteral( "%1행(%2..%3)과 %4행(%5..%6)의 색이 겹친다" )
                                      .arg( row - 1 )
                                      .arg( extents.at( row - 1 ).top )
                                      .arg( extents.at( row - 1 ).bottom )
                                      .arg( row )
                                      .arg( extents.at( row ).top )
                                      .arg( extents.at( row ).bottom ) ) );
    }
}

void ThemeColorSwatchTest::translucentColorShowsTheCheckerboard()
{
    const QSize size( 32, 16 );
    QPalette    palette;
    palette.setColor( QPalette::Active, QPalette::Base, QColor( QStringLiteral( "#ffffff" ) ) );

    // 완전히 투명한 색이면 보이는 것은 체커보드뿐이다. 체커보드가 없으면 그림이
    // 통째로 비어(투명) 나오고, 그러면 알파가 다른 두 색을 화면에서 구분할 수 없다.
    const QImage image =
            mrst::panelicons::colorSwatch( QColor( 0, 0, 0, 0 ), size, palette ).pixmap( size ).toImage();
    QVERIFY( !image.isNull() );

    QSet< QRgb > tones;
    for( int y = 3; y < image.height() - 3; ++y )
    {
        for( int x = 3; x < image.width() - 3; ++x )
            tones.insert( image.pixel( x, y ) );
    }
    QVERIFY2( tones.size() >= 2,
              qPrintable( QStringLiteral( "체커보드가 단색이다(색 %1가지)" ).arg( tones.size() ) ) );
}

MRST_REGISTER_TEST( ThemeColorSwatchTest );

#include "tst_ThemeColorSwatch.moc"
