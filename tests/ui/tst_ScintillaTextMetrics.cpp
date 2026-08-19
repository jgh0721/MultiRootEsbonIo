#include "TestRunner.hpp"

#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QStringList>
#include <QTest>

#include <ScintillaEditBase.h>
#include <Scintilla.h>

#include "thirdparty/scintilla-qt/PlatQtMetrics.hpp"

#include <algorithm>
#include <utility>
#include <vector>

// 캐시 모드를 넓혔을 때 메모리를 얼마나 더 먹는지 보려면 프로세스 작업 집합을
// 읽어야 한다. Qt 에는 대응물이 없다.
#include <windows.h>
#include <psapi.h>

/// 폭 측정(`Surface::MeasureWidths`) 비용을 재는 계측 하네스.
///
/// `PlatQt.cpp` 의 `SurfaceImpl::MeasureWidths` 는 문자마다 `QTextLine::cursorToX`
/// 를 부른다. 그 함수는 **매 호출이 줄 시작부터 advance 를 다시 누적**하므로
/// (qtextlayout.cpp 의 prefix 합산 루프에 캐시도 증분 상태도 없다) 줄 길이에 대해
/// O(n^2) 이다. 별도 실측에서 2000줄 폭 측정 104ms 중 shaping 은 43ms 뿐이고
/// 나머지 61ms 가 이 루프였다.
///
/// 이 비용이 화면 밖까지 번지는 이유는 **자동 줄바꿈이 기본으로 켜져 있기**
/// 때문이다(`ScintillaEditorSettings.hpp` 의 `wrapMode = WrapChar`). wrap 이 켜지면
/// `Editor::WrapBlock` 이 문서 전 줄에 `LayoutLine` 을 돌린다.
///
/// 여기서는 그 전체 wrap 패스를 재서, 개선 전후를 같은 잣대로 비교할 수 있게
/// 한다. 벽시계 값이라 CI 에서 단언하지 않는다 — `MRST_PERF` 를 준 사람만 돌린다.
class ScintillaTextMetricsTest : public QObject
{
    Q_OBJECT

private slots:
    void                                positionsAgreeAcrossPaths();
    void                                fullWrapCost();
    void                                layoutCacheModeCost();
};

namespace {

constexpr int kFixtureLines = 2000;
constexpr int kRepeat       = 5;
constexpr int kPointSize    = 11;
constexpr int kViewWidth    = 900;
constexpr int kViewHeight   = 600;

/// 픽스처. 파일로 커밋하지 않고 여기서 만든다 — 릴리스 ZIP 에 파일이 늘면
/// `PackageRelease.cmake` 의 목록 검증까지 따라 고쳐야 한다.
enum class Corpus
{
    AsciiReST,   ///< 코어 지름길(ASCII 전용 세그먼트)이 잡는 쪽
    Hangul,      ///< 전각 CJK 지름길이 잡는 쪽
    Mixed,       ///< 한 줄 안에서 갈리는 쪽
    Realistic,   ///< 실제 한국어 기술문서 — ※ ○ → ℃ 같은 기호가 섞인다
};

QByteArray buildCorpus( const Corpus corpus )
{
    static const char* asciiTpl[] = {
        "``ScintillaQtDirectBackend::applySettings`` sends the wrap messages only",
        ".. note:: The position cache stores segments shorter than 30 bytes only.",
        "   * :ref:`text-view-settings` -- see also :doc:`../guide/editor`",
        "+------------------+---------------------+------------------------+",
        "The quick brown fox jumps over the lazy dog while measuring widths.",
        "int width = compute(a, b) + (c * d) - e / f;  /* mixed punctuation */",
    };
    static const char* hangulTpl[] = {
        "이 문단은 폭 측정 비용을 재기 위한 한국어 산문이다. 세그먼트가 길어진다.",
        "자동 줄바꿈이 켜져 있으면 화면 밖 줄까지 폭을 재야 하므로 비용이 번진다.",
        "위치 캐시는 삼십 바이트 미만만 저장하므로 긴 문단은 매번 다시 측정된다.",
        "한글은 아스키가 아니어서 등폭 지름길이 잡아 주지 못하는 구간에 들어간다.",
        "문서를 열 때와 창 크기를 바꿀 때 이 비용이 한꺼번에 몰려서 나타난다.",
        "측정과 그리기가 같은 값을 써야 캐럿과 선택 범위가 어긋나지 않는다.",
    };
    static const char* mixedTpl[] = {
        "``wrapMode`` 값이 WrapChar 이므로 줄바꿈이 기본으로 켜져 있다.",
        "위치 캐시(:cpp:class:`PositionCache`)는 30 바이트 미만만 담는다.",
        "`Editor::WrapBlock` 이 문서 전 줄에 ``LayoutLine`` 을 돌린다.",
        "폰트가 등폭이면 ``monospaceASCII`` 판정이 참이 되어 곱셈으로 끝난다.",
        "reST 의 :ref:`참조` 와 한글 산문이 한 줄에 섞이면 런이 갈린다.",
        "``QTextLine::cursorToX`` 는 매번 줄 처음부터 다시 누적한다 — O(n^2).",
    };

    // 한글 자판에서 초성 + 한자키로 넣는 기호들. 유니코드로는 흩어져 있고
    // (※ U+203B, ○ U+25CB, → U+2192, ℃ U+2103, 「」 U+300C/D, ± U+00B1)
    // 동아시아 폭이 Ambiguous 라 폰트마다 반각/전각이 갈린다. 실제 한국어
    // 기술문서에는 흔하다.
    static const char* realisticTpl[] = {
        "※ 주의: 이 값을 바꾸면 기존 백업이 고아가 된다 (0.4.0 에서 실측).",
        "○ 입력 범위는 −40 ℃ ~ +85 ℃ 이며, 오차는 ±0.5 ℃ 이내여야 한다.",
        "「설정」 → 「편집기」 → 「글꼴」 에서 고른다. 기본값은 ``Consolas`` 다.",
        "· 항목 ① 은 필수, ② 와 ③ 은 선택이다. ※ ①~③ 은 배타적이지 않다.",
        "면적 ㎡ 와 부피 ㎥ 단위를 쓴다. 1 ㎢ = 1,000,000 ㎡ 이다.",
        "『참고 문헌』 § 3.2 “측정 방법” 을 볼 것 — 오차 ≤ 0.1 % 로 맞춘다.",
    };

    const char* const* tpl  = asciiTpl;
    if( corpus == Corpus::Hangul )
        tpl = hangulTpl;
    else if( corpus == Corpus::Mixed )
        tpl = mixedTpl;
    else if( corpus == Corpus::Realistic )
        tpl = realisticTpl;

    QByteArray text;
    text.reserve( kFixtureLines * 128 );
    for( int i = 0; i < kFixtureLines; ++i )
    {
        text += tpl[ i % 6 ];
        text += QByteArray( " (" ) + QByteArray::number( i ) + ')';
        if( i + 1 < kFixtureLines )
            text += '\n';
    }
    return text;
}

QString corpusName( const Corpus corpus )
{
    switch( corpus )
    {
        case Corpus::AsciiReST: return QStringLiteral( "ASCII" );
        case Corpus::Hangul:    return QStringLiteral( "한글" );
        case Corpus::Mixed:     return QStringLiteral( "혼합" );
        case Corpus::Realistic: return QStringLiteral( "실전" );
    }
    return QStringLiteral( "?" );
}

double median( std::vector< double > values )
{
    if( values.empty() )
        return 0.0;
    std::sort( values.begin(), values.end() );
    return values[ values.size() / 2 ];
}

struct Knobs
{
    bool checkMonospaced = false;
    int  layoutCache     = SC_CACHE_PAGE;
    int  positionCache   = 1024;   ///< Scintilla 기본값
    /// ASCII 연속 구간을 별도 스타일로 칠한다. 렉서가 마크업을 다르게 칠하는
    /// 상황을 흉내내려는 것이다 — 등폭 지름길은 **세그먼트** 단위로 걸리고,
    /// 세그먼트는 BreakFinder 가 스타일 경계에서 자른다. 스타일이 하나뿐이면
    /// 줄 하나가 세그먼트 하나여서, 한글이 한 글자만 있어도 그 줄이 통째로
    /// 탈락한다. 실제 문서는 그렇지 않다.
    bool styleAsciiRuns  = false;
};

/// ASCII / 비ASCII 가 바뀌는 지점마다 스타일을 갈아 준다.
///
/// 이것은 렉서 커버리지의 **상한**이다. 실제 reST 렉서는 ``literal`` 이나
/// :ref: 같은 토큰에만 다른 스타일을 주지 산문 속 영단어까지 갈라 주지는
/// 않는다. 즉 실제 값은 이 열과 스타일 없는 열 사이 어딘가에 있다.
void styleAsciiRunsIn( ScintillaEditBase& editor, const QByteArray& text )
{
    editor.send( SCI_STARTSTYLING, 0 );
    const char* const data  = text.constData();
    const int         total = text.size();
    int               i     = 0;
    while( i < total )
    {
        const auto graphicAscii = []( const char c ) {
            const unsigned char u = static_cast< unsigned char >( c );
            return u >= 0x20 && u <= 0x7E;
        };
        const bool ascii = graphicAscii( data[ i ] );
        int        j     = i;
        while( j < total && graphicAscii( data[ j ] ) == ascii )
            ++j;
        editor.send( SCI_SETSTYLING, j - i, ascii ? 1 : 0 );
        i = j;
    }
}

struct Sample
{
    double             ms    = 0;
    unsigned long long calls = 0;   ///< 그 사이 Surface::MeasureWidths 가 불린 횟수
};

/// 문서 전체를 wrap 하는 데 드는 시간(ms)과 플랫폼 측정 호출 횟수.
///
/// `SCI_VISIBLEFROMDOCLINE` 은 `pcs->DisplayFromDoc()` 만 부르고 wrap 을 강제하지
/// **않는다**. 전체 wrap 을 끌어내는 것은 `SCI_ENSUREVISIBLE` 이다 —
/// `Editor::EnsureLineVisible` 이 "In case in need of wrapping to ensure
/// DisplayFromDoc works" 주석과 함께 `WrapLines(WrapScope::wsAll)` 을 부른다.
Sample measureFullWrapMs( ScintillaEditBase& editor,
                          const QString&     fontFamily,
                          const QByteArray&  text,
                          const int          lineCount,
                          const Knobs&       knobs )
{
    const QByteArray family = fontFamily.toUtf8();

    std::vector< double > samples;
    samples.reserve( kRepeat );
    unsigned long long lastCalls = 0;
    for( int round = 0; round < kRepeat; ++round )
    {
        // 매 회차마다 캐시를 비운다. SCI_SETTEXT 가 줄 레이아웃 캐시를 무효화하고,
        // 위치 캐시는 크기를 다시 넣으면 벡터가 재할당되며 비워진다.
        editor.send( SCI_SETWRAPMODE, SC_WRAP_NONE );
        editor.send( SCI_SETLAYOUTCACHE, knobs.layoutCache );
        editor.send( SCI_SETPOSITIONCACHE, knobs.positionCache );

        editor.send( SCI_STYLESETCHECKMONOSPACED, STYLE_DEFAULT, knobs.checkMonospaced ? 1 : 0 );
        editor.sends( SCI_STYLESETFONT, STYLE_DEFAULT, family.constData() );
        editor.send( SCI_STYLESETSIZE, STYLE_DEFAULT, kPointSize );
        editor.send( SCI_STYLECLEARALL );

        editor.sends( SCI_SETTEXT, 0, text.constData() );
        if( knobs.styleAsciiRuns )
            styleAsciiRunsIn( editor, text );
        // 첫 화면 레이아웃과 등폭 프로브를 먼저 소진시켜 측정에서 뺀다.
        editor.send( SCI_ENSUREVISIBLE, 0 );

        // 등폭 지름길이 실제로 걸렸는지는 시간이 아니라 이 숫자가 말해 준다.
        // Scintilla 가 판정 결과를 알려주지 않으므로 플랫폼 계층에서 세는 수밖에
        // 없다 (PlatQtMetrics.hpp 의 설명 참고).
        auto&                    counter = mrst::scintilla::measureWidthsCallCount();
        const unsigned long long before  = counter.load( std::memory_order_relaxed );

        QElapsedTimer timer;
        timer.start();
        editor.send( SCI_SETWRAPMODE, SC_WRAP_CHAR );
        editor.send( SCI_ENSUREVISIBLE, lineCount - 1 );
        samples.push_back( timer.nsecsElapsed() / 1e6 );

        lastCalls = counter.load( std::memory_order_relaxed ) - before;
    }
    return { median( std::move( samples ) ), lastCalls };
}

/// 한 줄 문서를 넣고 모든 문서 위치의 화면 x 를 모은다.
///
/// `optimised` 가 참이면 운영 설정(코어의 ASCII 지름길 + 우리 전각 CJK 지름길),
/// 거짓이면 둘 다 끈 순수 폴백이다. 두 결과가 같아야 한다.
QList< int > collectPositions( ScintillaEditBase& editor,
                               const QString&     fontFamily,
                               const QByteArray&  line,
                               const bool         optimised )
{
    mrst::scintilla::setMonospaceFastPathEnabled( optimised );

    // 캐시가 앞선 회차의 값을 돌려주면 두 경로를 견주는 의미가 없어진다.
    editor.send( SCI_SETLAYOUTCACHE, SC_CACHE_NONE );
    editor.send( SCI_SETPOSITIONCACHE, 0 );
    editor.send( SCI_SETWRAPMODE, SC_WRAP_NONE );

    const QByteArray family = fontFamily.toUtf8();
    editor.send( SCI_STYLESETCHECKMONOSPACED, STYLE_DEFAULT, optimised ? 1 : 0 );
    editor.sends( SCI_STYLESETFONT, STYLE_DEFAULT, family.constData() );
    editor.send( SCI_STYLESETSIZE, STYLE_DEFAULT, kPointSize );
    editor.send( SCI_STYLECLEARALL );

    editor.sends( SCI_SETTEXT, 0, line.constData() );
    editor.send( SCI_GOTOPOS, 0 );

    QList< int > xs;
    const int    len = static_cast< int >( editor.send( SCI_GETLENGTH ) );
    xs.reserve( len + 1 );
    for( int p = 0; p <= len; ++p )
        xs << static_cast< int >( editor.send( SCI_POINTXFROMPOSITION, 0, p ) );
    return xs;
}

}  // namespace

/// 지름길이 사용자가 보는 것을 바꾸지 않는지 지킨다.
///
/// 폭 계산이 틀리면 캐럿이 글자 사이에 어긋나 앉고, 선택 범위가 밀리고, 세로
/// 눈금자가 글자와 안 맞는다. 그런데 그 증상은 "이 파일에서만 한 픽셀 이상하다"
/// 처럼 나타나서 눈으로 원인을 짚기가 몹시 어렵다. 그래서 최적화를 켠 결과와
/// 끈 결과를 직접 견준다 — 이것이 이 최적화의 유일한 안전장치다.
///
/// 특히 다음을 노린다.
///   - 전각 CJK 지름길이 쓰는 폭이 실제 shaping 결과와 같은가
///   - 안전 범위 밖 문자(※ ○ → ℃ 같은 초성+한자 기호)에서 제대로 폴백하는가
///   - 커닝이 있는 폰트가 등폭 프로브를 통과해 버리지 않는가
void ScintillaTextMetricsTest::positionsAgreeAcrossPaths()
{
    struct Case
    {
        const char* name;
        const char* utf8;
    };
    static const Case cases[] = {
        { "ASCII",          "if (a->b) { x = 1; }  // trailing comment" },
        { "한글",            "이 문단은 한국어 산문이다. 폭 측정을 검증한다." },
        { "한영 혼합",        "reST 에서 ``literal`` 과 한글이 한 줄에 섞인 경우" },
        { "특수문자",         "※ 참고: ○ → ℃ 「인용」 ‘따옴표’ … ― ± × ÷ ≠ ㎡" },
        { "한자",            "漢字 混用 文書 測定 結果" },
        { "호환 자모",        "ㄱㄴㄷㄹㅁㅂㅅㅇㅈㅊㅋㅌㅍㅎ ㅏㅑㅓㅕㅗㅛㅜㅠㅡㅣ" },
        { "조합 자모",        "\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8 조합형 자모" },
        { "리가처",           "-> => != <=> fi fl ffi === !== >>=" },
        { "결합 문자",        "e\xCC\x81 cafe\xCC\x81 nai\xCC\x88ve" },
        { "soft hyphen",    "soft\xC2\xAD" "hyphen\xC2\xAD" "test" },
        { "이모지",           "emoji \xF0\x9F\x98\x8A \xF0\x9F\x94\xA5 done" },
        { "후행 공백",        "trailing spaces   " },
        { "탭",              "\tindented\twith\ttabs" },
        { "전각 기호",        "＄％＆＊ ！？ ，．" },
        { "가나",            "ひらがな カタカナ ｶﾀｶﾅ" },
    };

    QStringList fonts;
    for( const QString& candidate : { QStringLiteral( "Consolas" ),
                                      QStringLiteral( "Arial" ),
                                      QStringLiteral( "맑은 고딕" ),
                                      QStringLiteral( "Cascadia Code" ),
                                      QStringLiteral( "나눔고딕코딩" ),
                                      QStringLiteral( "Sarasa Mono K" ) } )
    {
        if( QFontDatabase::families().contains( candidate ) )
            fonts << candidate;
    }
    if( fonts.isEmpty() )
        QSKIP( "검증에 쓸 폰트가 하나도 설치되어 있지 않다" );

    ScintillaEditBase editor;
    editor.resize( kViewWidth, kViewHeight );
    editor.show();
    QApplication::processEvents();
    editor.send( SCI_SETCODEPAGE, SC_CP_UTF8 );
    editor.send( SCI_SETMARGINWIDTHN, 0, 0 );
    editor.send( SCI_SETMARGINWIDTHN, 1, 0 );
    editor.send( SCI_SETMARGINWIDTHN, 2, 0 );

    QStringList problems;
    for( const QString& font : std::as_const( fonts ) )
    {
        for( const Case& c : cases )
        {
            const QByteArray   line = QByteArray( c.utf8 );
            const QList< int > fast = collectPositions( editor, font, line, true );
            const QList< int > slow = collectPositions( editor, font, line, false );

            if( fast.size() != slow.size() )
            {
                problems << QStringLiteral( "  [%1 / %2] 위치 개수가 다르다 (%3 vs %4)" )
                                .arg( font, QString::fromUtf8( c.name ) )
                                .arg( fast.size() )
                                .arg( slow.size() );
                continue;
            }

            for( int i = 0; i < fast.size(); ++i )
            {
                if( fast[ i ] != slow[ i ] )
                {
                    problems << QStringLiteral(
                                    "  [%1 / %2] 바이트 %3 의 x 가 다르다: 지름길 %4 px, 폴백 %5 px" )
                                    .arg( font, QString::fromUtf8( c.name ) )
                                    .arg( i )
                                    .arg( fast[ i ] )
                                    .arg( slow[ i ] );
                    break;
                }
            }

            for( int i = 1; i < fast.size(); ++i )
            {
                if( fast[ i ] < fast[ i - 1 ] )
                {
                    problems << QStringLiteral(
                                    "  [%1 / %2] x 가 뒤로 간다 (바이트 %3: %4 → %5)."
                                    " 이러면 캐럿 이분탐색이 엉킨다" )
                                    .arg( font, QString::fromUtf8( c.name ) )
                                    .arg( i )
                                    .arg( fast[ i - 1 ] )
                                    .arg( fast[ i ] );
                    break;
                }
            }
        }
    }

    // 다음 테스트가 영향을 받지 않도록 되돌린다.
    mrst::scintilla::setMonospaceFastPathEnabled( true );

    if( !problems.isEmpty() )
        QFAIL( qPrintable( QStringLiteral(
                               "등폭 지름길이 폴백과 다른 문자 위치를 낸다 (%1건).\n"
                               "그대로 두면 캐럿이 글자 사이에 어긋나 앉고 선택 범위가 밀린다.\n%2" )
                               .arg( problems.size() )
                               .arg( problems.join( QLatin1Char( '\n' ) ) ) ) );
}

void ScintillaTextMetricsTest::fullWrapCost()
{
    if( qgetenv( "MRST_PERF" ).trimmed().isEmpty() )
        QSKIP( "성능 계측은 MRST_PERF=1 일 때만 돈다 (벽시계 값이라 CI 에서 단언하지 않는다)" );

    // 등폭 / 비례 두 가지를 본다. 비례 폰트 열은 등폭 지름길이 원리적으로 못 돕는
    // 구간이라, 플랫폼 계층을 고쳐야 하는지 판단하는 잣대가 된다.
    QStringList fonts;
    for( const QString& candidate : { QStringLiteral( "Consolas" ),
                                      QStringLiteral( "Arial" ),
                                      QStringLiteral( "Cascadia Code" ) } )
    {
        if( QFontDatabase::families().contains( candidate ) )
            fonts << candidate;
    }
    if( fonts.isEmpty() )
        QSKIP( "측정에 쓸 폰트가 하나도 설치되어 있지 않다" );

    ScintillaEditBase editor;
    editor.resize( kViewWidth, kViewHeight );
    editor.show();
    QApplication::processEvents();
    editor.send( SCI_SETCODEPAGE, SC_CP_UTF8 );
    editor.send( SCI_SETMARGINWIDTHN, 0, 0 );
    editor.send( SCI_SETMARGINWIDTHN, 1, 0 );
    editor.send( SCI_SETMARGINWIDTHN, 2, 0 );

    qInfo().noquote() << QStringLiteral( "\n전체 wrap 비용 (%1줄, %2x%3, %4pt, %5회 중앙값)" )
                             .arg( kFixtureLines ).arg( kViewWidth ).arg( kViewHeight )
                             .arg( kPointSize ).arg( kRepeat );
    qInfo().noquote() << QStringLiteral( "괄호 안은 그 사이 Surface::MeasureWidths 가 불린 횟수 —"
                                         " 0 이면 등폭 지름길이 전부 먹었다는 뜻이다.\n"
                                         "'스타일' 열은 ASCII 런마다 스타일을 갈아 준 것으로,"
                                         " 렉서가 마크업을 다르게 칠하는 상황의 상한이다." );
    qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4 %5 %6" )
                             .arg( QStringLiteral( "폰트" ), -16 )
                             .arg( QStringLiteral( "말뭉치" ), -8 )
                             .arg( QStringLiteral( "기본" ), 20 )
                             .arg( QStringLiteral( "등폭만" ), 20 )
                             .arg( QStringLiteral( "스타일만" ), 20 )
                             .arg( QStringLiteral( "등폭+스타일" ), 20 );

    const auto cell = []( const Sample& s ) {
        return QStringLiteral( "%1 (%2)" )
            .arg( QString::number( s.ms, 'f', 1 ) )
            .arg( s.calls );
    };

    for( const QString& font : std::as_const( fonts ) )
    {
        for( const Corpus corpus :
             { Corpus::AsciiReST, Corpus::Hangul, Corpus::Mixed, Corpus::Realistic } )
        {
            const QByteArray text = buildCorpus( corpus );

            const Sample plain = measureFullWrapMs( editor, font, text, kFixtureLines, Knobs{} );

            Knobs mono;
            mono.checkMonospaced = true;
            const Sample withFastPath = measureFullWrapMs( editor, font, text, kFixtureLines, mono );

            // 스타일만 갈라 본다. 세그먼트가 잘게 나뉘는 것만으로도 위치 캐시의
            // 30바이트 문턱을 넘어서므로, 지름길과 무관한 이득이 여기 섞여 있다.
            // 그걸 분리해야 등폭 지름길의 순효과를 말할 수 있다.
            Knobs styledOnly;
            styledOnly.styleAsciiRuns = true;
            const Sample withStylesOnly = measureFullWrapMs( editor, font, text, kFixtureLines, styledOnly );

            // 렉서가 스타일을 갈라 놓았을 때 ASCII 세그먼트가 지름길을 타는지 본다.
            // 이게 한글이 섞인 실제 문서에서 지름길이 얼마나 먹히는지의 상한이다.
            Knobs monoStyled          = mono;
            monoStyled.styleAsciiRuns = true;
            const Sample withStyles = measureFullWrapMs( editor, font, text, kFixtureLines, monoStyled );

            qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4 %5 %6" )
                                     .arg( font, -16 )
                                     .arg( corpusName( corpus ), -8 )
                                     .arg( cell( plain ), 20 )
                                     .arg( cell( withFastPath ), 20 )
                                     .arg( cell( withStylesOnly ), 20 )
                                     .arg( cell( withStyles ), 20 );
        }
    }

    // 단언은 하나뿐이다 — 측정이 실제로 일어났는지. 벽시계 임계값을 넣으면
    // 빌드 기계가 바쁜 날 빨간불이 뜨고, 그 빨간불은 아무도 고치지 않는다.
    QVERIFY( editor.send( SCI_GETLINECOUNT ) >= kFixtureLines );
}

/// 줄 레이아웃 캐시를 문서 전체로 넓히는 것이 값을 하는지 잰다.
///
/// `SC_CACHE_PAGE` 에서는 `SignificantLines::LineMayCache` 가 화면 밖 줄을 걸러서,
/// 전체 wrap 패스가 **아무것도 캐시하지 않고** llTemporary 를 재사용한다. 그래서
/// 창 크기가 바뀌어 다시 wrap 할 때마다 처음부터 다 계산한다. `SC_CACHE_DOCUMENT`
/// 는 그걸 남기지만 대가로 줄 수에 비례하는 메모리를 먹는다.
///
/// 폭 측정 자체가 싸진 뒤로는 이 교환의 무게가 달라졌을 수 있어 다시 잰다.
void ScintillaTextMetricsTest::layoutCacheModeCost()
{
    if( qgetenv( "MRST_PERF" ).trimmed().isEmpty() )
        QSKIP( "성능 계측은 MRST_PERF=1 일 때만 돈다" );
    if( !QFontDatabase::families().contains( QStringLiteral( "Consolas" ) ) )
        QSKIP( "Consolas 가 없다" );

    const QByteArray text = buildCorpus( Corpus::Realistic );

    qInfo().noquote() << QStringLiteral( "\n줄 레이아웃 캐시 모드 (%1줄, 실전 말뭉치, Consolas %2pt)" )
                             .arg( kFixtureLines ).arg( kPointSize );
    qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4" )
                             .arg( QStringLiteral( "모드" ), -14 )
                             .arg( QStringLiteral( "첫 wrap" ), 12 )
                             .arg( QStringLiteral( "재-wrap" ), 12 )
                             .arg( QStringLiteral( "작업집합 증가" ), 16 );

    for( const auto& mode : { std::pair< int, const char* >{ SC_CACHE_PAGE, "PAGE" },
                              std::pair< int, const char* >{ SC_CACHE_DOCUMENT, "DOCUMENT" } } )
    {
        ScintillaEditBase editor;
        editor.resize( kViewWidth, kViewHeight );
        editor.show();
        QApplication::processEvents();
        editor.send( SCI_SETCODEPAGE, SC_CP_UTF8 );
        editor.send( SCI_SETMARGINWIDTHN, 0, 0 );
        editor.send( SCI_SETMARGINWIDTHN, 1, 0 );
        editor.send( SCI_SETMARGINWIDTHN, 2, 0 );
        editor.send( SCI_SETLAYOUTCACHE, mode.first );
        editor.send( SCI_STYLESETCHECKMONOSPACED, STYLE_DEFAULT, 1 );
        editor.sends( SCI_STYLESETFONT, STYLE_DEFAULT, "Consolas" );
        editor.send( SCI_STYLESETSIZE, STYLE_DEFAULT, kPointSize );
        editor.send( SCI_STYLECLEARALL );

        PROCESS_MEMORY_COUNTERS before{};
        before.cb = sizeof( before );
        GetProcessMemoryInfo( GetCurrentProcess(), &before, sizeof( before ) );

        editor.send( SCI_SETWRAPMODE, SC_WRAP_NONE );
        editor.sends( SCI_SETTEXT, 0, text.constData() );
        editor.send( SCI_ENSUREVISIBLE, 0 );

        QElapsedTimer timer;
        timer.start();
        editor.send( SCI_SETWRAPMODE, SC_WRAP_CHAR );
        editor.send( SCI_ENSUREVISIBLE, kFixtureLines - 1 );
        const double firstMs = timer.nsecsElapsed() / 1e6;

        // 텍스트는 그대로 두고 wrap 만 다시 시킨다. 창 크기를 바꿨을 때와 같은 일이
        // 벌어지는데, 캐시가 살아 있으면 여기서 값을 한다.
        std::vector< double > again;
        for( int round = 0; round < kRepeat; ++round )
        {
            editor.send( SCI_SETWRAPMODE, SC_WRAP_NONE );
            timer.restart();
            editor.send( SCI_SETWRAPMODE, SC_WRAP_CHAR );
            editor.send( SCI_ENSUREVISIBLE, kFixtureLines - 1 );
            again.push_back( timer.nsecsElapsed() / 1e6 );
        }

        PROCESS_MEMORY_COUNTERS after{};
        after.cb = sizeof( after );
        GetProcessMemoryInfo( GetCurrentProcess(), &after, sizeof( after ) );
        const double grewMiB =
            ( double( after.WorkingSetSize ) - double( before.WorkingSetSize ) ) / ( 1024.0 * 1024.0 );

        qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4" )
                                 .arg( QString::fromLatin1( mode.second ), -14 )
                                 .arg( QString::number( firstMs, 'f', 1 ), 12 )
                                 .arg( QString::number( median( again ), 'f', 1 ), 12 )
                                 .arg( QStringLiteral( "%1 MiB" ).arg( grewMiB, 0, 'f', 1 ), 16 );
    }

    QVERIFY( true );
}

MRST_REGISTER_TEST( ScintillaTextMetricsTest );

#include "tst_ScintillaTextMetrics.moc"
