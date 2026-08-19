#include "TestRunner.hpp"

#include "core/solRstOfflineCompletions.hpp"
#include "core/solThemeManager.hpp"
#include "editor/CompletionDetailPopup.hpp"
#include "editor/CompletionPopupWidget.hpp"

#include <QImage>

#include <cmath>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTest>

using namespace mrst;

/// 위젯을 실제로 만들어 두드린다.
///
/// SendInput/PrintWindow 로 하던 확인의 대부분이 여기로 온다. 이쪽이 나은 이유:
///
/// * **포커스가 필요 없다.** QTest 는 수신 위젯을 이름으로 지정한다. 화면 자동화가
///   가장 자주 미끄러지는 곳이 바로 실제 OS 포커스를 유도하는 대목이었다.
/// * **횟수를 셀 수 있다.** "상세 패널이 깜빡인다" 를 QSignalSpy 로 숫자로 고정한다.
///   창 캡처 쪽은 10ms 폴링이라 경합적이다.
/// * **기하가 정확하다.** intersects() 한 줄이면 "패널이 목록을 덮는가" 가 끝난다.
/// * **grab() 은 컴포지터를 거치지 않는다.** DWM 이 끼어들어 실제와 다른 색이
///   찍히는 일이 없다.
///
/// 반대로 여기서 **못 하는 것**은 남겨 둔다: 실제 활성화·z-order(우리 팝업은
/// Qt::Tool + WA_ShowWithoutActivating 이라 창 관리자 동작에 기댄다), 실제 IME,
/// DWM 프레임. 그건 여전히 손으로 확인한다.
class TestCompletionPopupUi : public QObject
{
    Q_OBJECT

private slots:
    void ordersByFuzzyScoreThenBias();
    void emitsCurrentItemChangedOnceWhenRefilling();
    void keepsAnchorWhenGeometryRefreshed();
    void hidesWhenFilterMatchesNothing();
    void wideningTheFilterRestoresTheFullList();
    void navigatesWithArrowKeysAndAcceptsWithEnter();

    void detailPanelDoesNotOverlapTheList();
    void filePanelKeepsFixedSizeAcrossItems();
    void stalePreviewIsIgnored();

    void accentAndMutedColorsAreReadableInBothThemes();

private:
    [[nodiscard]] static CompletionDisplayItem item( const QString& label, int scoreBias = 0,
                                                     const QString& detail = {} );
    [[nodiscard]] static QStringList labelsOf( const CompletionPopupWidget& popup );
};

CompletionDisplayItem TestCompletionPopupUi::item( const QString& label, const int scoreBias,
                                                   const QString& detail )
{
    CompletionDisplayItem value;
    value.label = label;
    value.insertText = detail.isEmpty() ? label : detail + QLatin1Char( '/' ) + label;
    value.detail = detail;
    value.kind = 17;
    value.filterText = label;
    value.scoreBias = scoreBias;
    return value;
}

QStringList TestCompletionPopupUi::labelsOf( const CompletionPopupWidget& popup )
{
    QStringList labels;
    // currentItem() 만으로는 순서를 못 본다. 선택을 돌려 가며 읽는다.
    auto& mutablePopup = const_cast< CompletionPopupWidget& >( popup );
    for( int index = 0; index < popup.visibleCount(); ++index )
    {
        labels << mutablePopup.currentItem().label;
        mutablePopup.selectNext();
    }
    return labels;
}

void TestCompletionPopupUi::ordersByFuzzyScoreThenBias()
{
    CompletionPopupWidget popup;
    // 전역 후보(가중치 0)의 이름이 더 짧아도, 지금 보고 있는 디렉터리(+30)가
    // 위로 와야 한다. 이 판단은 공급자만 할 수 있으므로 가중치로 넘긴다.
    popup.setItems( { item( QStringLiteral( "cover-wide.png" ), 0, QStringLiteral( "far" ) ),
                     item( QStringLiteral( "cover.png" ), 30 ) } );
    popup.updateFilter( QStringLiteral( "cover" ) );

    QCOMPARE( popup.visibleCount(), 2 );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "cover.png" ) );
}

void TestCompletionPopupUi::emitsCurrentItemChangedOnceWhenRefilling()
{
    CompletionPopupWidget popup;
    popup.setItems( { item( QStringLiteral( "alpha.png" ) ), item( QStringLiteral( "beta.png" ) ) } );

    QSignalSpy spy( &popup, &CompletionPopupWidget::currentItemChanged );

    // 같은 첫 항목이 유지되는 갱신. 예전에는 clear() 가 빈 항목을 흘리고
    // selectFirst() 가 다시 흘려 상세 패널이 껐다 켜졌다.
    popup.setItems( { item( QStringLiteral( "alpha.png" ) ), item( QStringLiteral( "gamma.png" ) ) } );
    QCOMPARE( spy.count(), 0 );

    // 실제로 바뀔 때는 정확히 한 번만.
    popup.updateFilter( QStringLiteral( "gam" ) );
    QCOMPARE( spy.count(), 1 );
}

void TestCompletionPopupUi::keepsAnchorWhenGeometryRefreshed()
{
    CompletionPopupWidget popup;
    popup.setItems( { item( QStringLiteral( "alpha.png" ) ) } );
    popup.showAt( QPoint( 120, 140 ) );
    QVERIFY( QTest::qWaitForWindowExposed( &popup ) );

    const QPoint before = popup.pos();

    // LSP 응답이 늦게 도착해 목록이 길어지는 상황. 예전에는 showPopupAtCaret() 이
    // **현재** 캐럿에 다시 붙여 목록이 옆으로 튀었다.
    popup.setItems( { item( QStringLiteral( "alpha.png" ) ), item( QStringLiteral( "beta.png" ) ),
                     item( QStringLiteral( "gamma.png" ) ) } );
    popup.refreshGeometry();

    QCOMPARE( popup.pos(), before );
    QVERIFY( popup.height() > 0 );
}

void TestCompletionPopupUi::hidesWhenFilterMatchesNothing()
{
    CompletionPopupWidget popup;
    popup.setItems( { item( QStringLiteral( "alpha.png" ) ) } );
    popup.showAt( QPoint( 60, 60 ) );
    QVERIFY( QTest::qWaitForWindowExposed( &popup ) );

    popup.updateFilter( QStringLiteral( "zzz" ) );
    QCOMPARE( popup.visibleCount(), 0 );
    QVERIFY( !popup.isVisible() );
}

void TestCompletionPopupUi::wideningTheFilterRestoresTheFullList()
{
    CompletionPopupWidget popup;
    popup.setItems( { item( QStringLiteral( "image" ) ), item( QStringLiteral( "figure" ) ),
                     item( QStringLiteral( "note" ) ) } );

    popup.updateFilter( QStringLiteral( "image" ) );
    QCOMPARE( popup.visibleCount(), 1 );

    // ".. image" 를 ".. " 로 지웠을 때 목록이 image 로 좁혀진 채 남으면 안 된다.
    // rebuild() 가 언제나 allItems_ 에서 다시 거르기 때문에 성립하는 계약이다.
    popup.updateFilter( QString{} );
    QCOMPARE( popup.visibleCount(), 3 );

    popup.updateFilter( QStringLiteral( "fig" ) );
    QCOMPARE( popup.visibleCount(), 1 );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "figure" ) );
}

void TestCompletionPopupUi::navigatesWithArrowKeysAndAcceptsWithEnter()
{
    CompletionPopupWidget popup;
    popup.setItems( { item( QStringLiteral( "alpha.png" ) ), item( QStringLiteral( "beta.png" ) ) } );
    // handleKeyPress 는 목록이 실제로 떠 있을 때만 키를 가로챈다.
    popup.showAt( QPoint( 80, 80 ) );
    QVERIFY( QTest::qWaitForWindowExposed( &popup ) );

    QSignalSpy accepted( &popup, &CompletionPopupWidget::itemSelected );

    QKeyEvent down( QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier );
    QVERIFY( popup.handleKeyPress( &down ) );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "beta.png" ) );

    QKeyEvent enter( QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier );
    QVERIFY( popup.handleKeyPress( &enter ) );
    QCOMPARE( accepted.count(), 1 );
    QCOMPARE( accepted.first().first().toString(), QStringLiteral( "beta.png" ) );
}

void TestCompletionPopupUi::detailPanelDoesNotOverlapTheList()
{
    CompletionPopupWidget popup;
    popup.setItems( { item( QStringLiteral( "a.png" ) ), item( QStringLiteral( "b.png" ) ) } );
    popup.showAt( QPoint( 40, 40 ) );
    QVERIFY( QTest::qWaitForWindowExposed( &popup ) );

    CompletionDetailPopup detail;
    CompletionFileDetail content;
    content.fileName = QStringLiteral( "a.png" );
    content.directoryText = QStringLiteral( "img/" );
    detail.setFileContent( content );
    detail.showBesideAnchor( popup.geometry() );
    QVERIFY( QTest::qWaitForWindowExposed( &detail ) );

    // 예전에는 오른쪽·왼쪽 둘만 보고 실패하면 화면 안으로 밀어 넣어 목록을 덮었다.
    QVERIFY( !detail.geometry().intersects( popup.geometry() ) );
}

void TestCompletionPopupUi::filePanelKeepsFixedSizeAcrossItems()
{
    CompletionDetailPopup detail;

    CompletionFileDetail shortOne;
    shortOne.fileName = QStringLiteral( "a.png" );
    shortOne.directoryText = QStringLiteral( "./" );
    detail.setFileContent( shortOne );
    const QSize first = detail.size();

    CompletionFileDetail longOne;
    longOne.fileName = QStringLiteral( "a-very-long-file-name-that-would-wrap.png" );
    longOne.directoryText =
        QStringLiteral( "../../../../Resources/Novel/Pt5/Vol12/deeper/and/deeper/" );
    longOne.metaLine = QStringLiteral( "PNG · 4096×4096 · 12.3 MB" );
    detail.setFileContent( longOne );

    // 방향키로 훑을 때 패널이 위아래로 펄떡이면 안 된다.
    QCOMPARE( detail.size(), first );
}

void TestCompletionPopupUi::stalePreviewIsIgnored()
{
    CompletionDetailPopup detail;
    detail.resize( detail.previewBoxSize() + QSize( 20, 120 ) );

    CompletionFileDetail first;
    first.fileName = QStringLiteral( "first.png" );
    const quint64 staleToken = detail.setFileContent( first );

    CompletionFileDetail second;
    second.fileName = QStringLiteral( "second.png" );
    detail.setFileContent( second );

    const QImage before = detail.grab().toImage();

    QPixmap late( 64, 64 );
    late.fill( Qt::red );
    detail.applyPreview( staleToken, late, false, QStringLiteral( "STALE" ), QString{} );

    // 늦게 도착한 프리뷰가 다른 항목 위에 붙으면 안 된다. QPointer 로는 못 막는다 —
    // 패널은 살아 있고 내용만 바뀐 경우가 정확히 이 버그다.
    QCOMPARE( detail.grab().toImage(), before );
}

void TestCompletionPopupUi::accentAndMutedColorsAreReadableInBothThemes()
{
    const auto luminance = []( const QColor& color ) {
        const auto channel = []( double value ) {
            value /= 255.0;
            return value <= 0.03928 ? value / 12.92 : std::pow( ( value + 0.055 ) / 1.055, 2.4 );
        };
        return 0.2126 * channel( color.red() ) + 0.7152 * channel( color.green() )
               + 0.0722 * channel( color.blue() );
    };
    const auto contrast = [ &luminance ]( const QColor& a, const QColor& b ) {
        const double first = luminance( a );
        const double second = luminance( b );
        return ( qMax( first, second ) + 0.05 ) / ( qMin( first, second ) + 0.05 );
    };

    ThemeManager& theme = ThemeManager::instance();
    const ThemeManager::Theme original = theme.currentTheme();

    for( const ThemeManager::Theme mode : { ThemeManager::Light, ThemeManager::Dark } )
    {
        theme.setTheme( mode );
        const QColor surface = theme.color( QStringLiteral( "common.surface" ) );
        const QColor accent = theme.color( QStringLiteral( "common.accent" ) );
        const QColor muted = theme.color( QStringLiteral( "common.foregroundMuted" ) );

        // 하드코딩하던 #1a73e8 은 다크에서 3.2:1 로 모자랐다. 퍼지 강조는 작은
        // 글씨가 아니므로 4.5:1 을 기준으로 본다.
        QVERIFY2( contrast( accent, surface ) >= 4.5,
                 qPrintable( QStringLiteral( "accent %1 on %2" )
                                 .arg( accent.name(), surface.name() ) ) );
        // 상세 열은 이제 장식이 아니라 동명 파일을 가르는 정보다. 읽혀야 한다.
        QVERIFY2( contrast( muted, surface ) >= 3.0,
                 qPrintable( QStringLiteral( "muted %1 on %2" )
                                 .arg( muted.name(), surface.name() ) ) );
    }

    theme.setTheme( original );
}

MRST_REGISTER_TEST( TestCompletionPopupUi );

#include "tst_CompletionPopupUi.moc"
