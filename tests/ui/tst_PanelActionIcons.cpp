#include "TestRunner.hpp"

#include "uis/PanelActionIcons.hpp"

#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QTest>

using namespace mrst;

namespace {

QPalette paletteWith( const QColor& buttonText, const QColor& disabledText )
{
    QPalette palette;
    palette.setColor( QPalette::Active, QPalette::ButtonText, buttonText );
    palette.setColor( QPalette::Disabled, QPalette::ButtonText, disabledText );
    return palette;
}

/// 밝은 테마: 검은 글자.
QPalette lightPalette()
{
    return paletteWith( QColor( 0x1a, 0x1a, 0x1a ), QColor( 0x9a, 0x9a, 0x9a ) );
}

/// 어두운 테마: 흰 글자.
QPalette darkPalette()
{
    return paletteWith( QColor( 0xe6, 0xe6, 0xe6 ), QColor( 0x70, 0x70, 0x70 ) );
}

struct IconEntry
{
    const char* name;
    QIcon ( *make )( const QPalette& );
};

const QVector< IconEntry >& allIcons()
{
    static const QVector< IconEntry > entries{
        { "newFile", &panelicons::newFile },     { "newFolder", &panelicons::newFolder },
        { "rename", &panelicons::rename },       { "remove", &panelicons::remove },
        { "filter", &panelicons::filter },
    };
    return entries;
}

/// 완전히 불투명한 픽셀 하나의 색. 안티에일리어싱을 거쳐도 획의 한가운데는
/// 요청한 색 그대로 남는다. 없으면 유효하지 않은 QColor.
QColor strokeColor( const QPixmap& pixmap )
{
    const QImage image = pixmap.toImage().convertToFormat( QImage::Format_ARGB32 );
    for( int y = 0; y < image.height(); ++y )
    {
        for( int x = 0; x < image.width(); ++x )
        {
            const QColor color = image.pixelColor( x, y );
            if( color.alpha() == 255 )
                return color;
        }
    }
    return {};
}

/// 채널마다 tolerance 안이면 같은 색으로 본다.
///
/// 정확히 같기를 요구하면 안 된다. 획이 겹치는 자리는 프리멀티플라이드 공간에서
/// 합성됐다가 되돌아오면서 채널이 1~2 흔들린다(새 파일 아이콘의 `+` 가 문서
/// 모서리에 닿는 자리가 그렇다). 우리가 잡으려는 것은 "테마 색을 따라가는가"
/// 이지 합성 반올림이 아니다.
bool colorsClose( const QColor& left, const QColor& right, const int tolerance = 3 )
{
    return qAbs( left.red() - right.red() ) <= tolerance
        && qAbs( left.green() - right.green() ) <= tolerance
        && qAbs( left.blue() - right.blue() ) <= tolerance;
}

int opaquePixelCount( const QPixmap& pixmap )
{
    const QImage image = pixmap.toImage().convertToFormat( QImage::Format_ARGB32 );
    int           count = 0;
    for( int y = 0; y < image.height(); ++y )
    {
        for( int x = 0; x < image.width(); ++x )
        {
            if( image.pixelColor( x, y ).alpha() > 0 )
                ++count;
        }
    }
    return count;
}

}  // namespace

/// 아이콘을 그려서 만들기 때문에 색이 팔레트를 따라간다는 사실이 곧 테마 지원의
/// 전부다. 그 연결이 끊어지면(색을 상수로 바꾼다든지) 어두운 테마에서 아이콘이
/// 배경에 묻혀 **보이지 않는 단추**가 되는데, 그것은 화면을 봐야만 드러난다.
/// 여기서 색을 직접 읽어 못 박아 둔다.
class TestPanelActionIcons : public QObject
{
    Q_OBJECT

private slots:
    void drawsSomethingAtEverySize_data();
    void drawsSomethingAtEverySize();
    void followsLightPalette_data();
    void followsLightPalette();
    void followsDarkPalette_data();
    void followsDarkPalette();
    void disabledDiffersFromNormal_data();
    void disabledDiffersFromNormal();
    void writeContactSheet();
};

void TestPanelActionIcons::drawsSomethingAtEverySize_data()
{
    QTest::addColumn< int >( "index" );
    for( int index = 0; index < allIcons().size(); ++index )
        QTest::newRow( allIcons().at( index ).name ) << index;
}

void TestPanelActionIcons::drawsSomethingAtEverySize()
{
    QFETCH( int, index );
    const QIcon icon = allIcons().at( index ).make( lightPalette() );

    QVERIFY( !icon.isNull() );
    for( const int size : { 16, 20, 24, 32 } )
    {
        const QPixmap pixmap = icon.pixmap( size, size );
        QVERIFY( !pixmap.isNull() );
        // **논리** 크기로 본다. QIcon::pixmap() 은 화면 배율만큼 키운 것을
        // 돌려주므로(125% 화면에서 16 을 청하면 20x20 이 온다) 픽셀 수로 비교하면
        // 이 테스트가 검사원의 모니터 설정에 달라붙는다.
        QCOMPARE( pixmap.deviceIndependentSize(), QSizeF( size, size ) );
        // 빈 도안은 없어야 한다. 경로를 잘못 닫으면 아무것도 안 그려진다.
        QVERIFY2( opaquePixelCount( pixmap ) > size, allIcons().at( index ).name );
    }
}

void TestPanelActionIcons::followsLightPalette_data()
{
    drawsSomethingAtEverySize_data();
}

void TestPanelActionIcons::followsLightPalette()
{
    QFETCH( int, index );
    const QColor wanted = lightPalette().color( QPalette::Active, QPalette::ButtonText );
    const QColor drawn = strokeColor( allIcons().at( index ).make( lightPalette() ).pixmap( 32, 32 ) );

    QVERIFY( drawn.isValid() );
    QVERIFY2( colorsClose( drawn, wanted ),
             qPrintable( QStringLiteral( "%1 != %2" ).arg( drawn.name(), wanted.name() ) ) );
}

void TestPanelActionIcons::followsDarkPalette_data()
{
    drawsSomethingAtEverySize_data();
}

void TestPanelActionIcons::followsDarkPalette()
{
    QFETCH( int, index );
    const QColor wanted = darkPalette().color( QPalette::Active, QPalette::ButtonText );
    const QColor drawn = strokeColor( allIcons().at( index ).make( darkPalette() ).pixmap( 32, 32 ) );

    QVERIFY( drawn.isValid() );
    QVERIFY2( colorsClose( drawn, wanted ),
             qPrintable( QStringLiteral( "%1 != %2" ).arg( drawn.name(), wanted.name() ) ) );
}

void TestPanelActionIcons::disabledDiffersFromNormal_data()
{
    drawsSomethingAtEverySize_data();
}

void TestPanelActionIcons::disabledDiffersFromNormal()
{
    QFETCH( int, index );
    const QIcon icon = allIcons().at( index ).make( darkPalette() );

    // Qt 가 유추해 주는 회색이 아니라 팔레트의 Disabled 색이어야 한다.
    // 다크 테마에서 유추된 회색은 배경과 붙어 사라진다.
    const QColor disabled =
        strokeColor( icon.pixmap( QSize( 32, 32 ), QIcon::Disabled, QIcon::Off ) );
    QVERIFY( disabled.isValid() );
    QVERIFY( colorsClose( disabled,
                         darkPalette().color( QPalette::Disabled, QPalette::ButtonText ) ) );
}

void TestPanelActionIcons::writeContactSheet()
{
    // 눈으로 볼 일이 있을 때만 쓴다. 도안을 고칠 때 이 한 장이면 다섯 개를
    // 두 테마에서 한꺼번에 확인할 수 있다.
    const QByteArray target = qgetenv( "MRST_ICON_SHEET" );
    if( target.isEmpty() )
        QSKIP( "MRST_ICON_SHEET 가 없다 (아이콘 대조표를 저장하지 않는다)" );

    constexpr int cell = 48;
    constexpr int pad = 8;
    const int     width = pad + static_cast< int >( allIcons().size() ) * ( cell + pad );

    QImage sheet( width, pad + 2 * ( cell + pad ), QImage::Format_ARGB32 );
    sheet.fill( Qt::transparent );

    QPainter painter( &sheet );
    for( int row = 0; row < 2; ++row )
    {
        const bool     dark = row == 1;
        const QPalette palette = dark ? darkPalette() : lightPalette();
        const int      y = pad + row * ( cell + pad );

        painter.fillRect( QRect( 0, y - pad / 2, width, cell + pad ),
                         dark ? QColor( 0x20, 0x21, 0x24 ) : QColor( 0xf5, 0xf5, 0xf5 ) );
        for( int column = 0; column < allIcons().size(); ++column )
        {
            const QPixmap pixmap = allIcons().at( column ).make( palette ).pixmap( 32, 32 );
            painter.drawPixmap( pad + column * ( cell + pad ) + ( cell - 32 ) / 2,
                               y + ( cell - 32 ) / 2, pixmap );
        }
    }
    painter.end();

    QVERIFY2( sheet.save( QString::fromLocal8Bit( target ) ), qPrintable( QString::fromLocal8Bit( target ) ) );
}

MRST_REGISTER_TEST( TestPanelActionIcons );

#include "tst_PanelActionIcons.moc"
