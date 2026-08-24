#include "TestRunner.hpp"

#include "editor/RstContainerLexer.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QTest>

#include <algorithm>
#include <string>
#include <vector>

using namespace mrst::rst;

namespace {

/// 회차 수. 벽시계 값이라 중앙값과 최솟값을 함께 낸다 — Windows 의 CPU 바운드
/// 마이크로벤치마크에서는 최솟값이 잡음이 가장 적다.
constexpr int kRepeat = 5;

/// 계측 결과를 여기에 흘려 최적화기가 렉싱 호출을 통째로 지워 버리는 것을 막는다.
volatile std::size_t g_sink = 0;

double median( std::vector< double > values )
{
    if( values.empty() )
        return 0.0;
    std::sort( values.begin(), values.end() );
    return values[ values.size() / 2 ];
}

double smallest( const std::vector< double >& values )
{
    return values.empty() ? 0.0 : *std::min_element( values.begin(), values.end() );
}

#if defined( _ITERATOR_DEBUG_LEVEL ) && _ITERATOR_DEBUG_LEVEL > 0
constexpr bool kDebugStl = true;
#else
constexpr bool kDebugStl = false;
#endif

/// MRST_PERF 가 없으면 건너뛴다. 최적화 빌드가 아니어도 건너뛴다 — 디버그 STL 에서
/// std::regex 는 릴리스 대비 10~50배 느려서, 재작성의 개선폭을 크게 과장한 수치가
/// 나온다. 잘못된 구성에서 잰 값이 문서에 실리는 사고를 여기서 막는다.
#define MRST_REQUIRE_PERF_BUILD()                                                                   \
    do                                                                                              \
    {                                                                                               \
        if( qgetenv( "MRST_PERF" ).trimmed().isEmpty() )                                            \
            QSKIP( "성능 계측은 MRST_PERF=1 일 때만 돈다 (벽시계 값이라 CI 에서 단언하지 않는다)" ); \
        if constexpr( kDebugStl )                                                                   \
            QSKIP( "최적화 빌드에서만 잰다. 디버그 STL 에서 std::regex 는 10~50배 느려 수치가 "     \
                   "무의미하다. RelWithDebInfo + MRST_BUILD_TESTS=ON 으로 다시 빌드할 것." );       \
    } while( false )

/// 실측 코퍼스 두 종류. 파일로 커밋하지 않고 여기서 만든다 — 릴리스 ZIP 에 파일이
/// 늘면 PackageRelease.cmake 의 목록 검증까지 따라 고쳐야 한다.
enum class Corpus
{
    /// 한국어 산문. 빈 줄 약 절반, 평균 70바이트/줄, 마크업 희박.
    /// 거의 모든 줄이 styleInline() 의 6회 정규식 스윕을 통과하고 아무것도 못 찾는다.
    Hangul,
    /// 기술 문서. directive·옵션 필드·인라인 리터럴이 조밀하다.
    Technical,
};

std::string buildCorpus( const Corpus kind, const int lines )
{
    std::string out;
    out.reserve( static_cast< std::size_t >( lines ) * 70 );

    if( kind == Corpus::Hangul )
    {
        static const char* body[] = {
            "프리다는 갈색 눈동자를 강하게 빛내며 기어코 올 것이 왔다고 생각했다.",
            "상업 길드에 계신 조부가 보낸 급사가 말을 타고 달려와 그렇게 말했다.",
            "함부로 건드렸다간 금방 깨질 것만 같은 오래된 마술구를 조심스레 집었다.",
            "\"마인은 처음으로 생긴 친구야. 우리 쪽으로 끌어들일 수 있다면 아깝지 않아.\"",
        };
        for( int i = 0; i < lines; ++i )
        {
            if( i % 2 == 1 )
            {
                out += '\n';
                continue;
            }
            if( i % 120 == 0 )
            {
                out += "제 " + std::to_string( i / 120 + 1 ) + " 화\n========\n";
                ++i;
                continue;
            }
            if( i % 40 == 0 )
            {
                out += "|br| |br|\n";
                continue;
            }
            out += body[ ( i / 2 ) % 4 ];
            out += '\n';
        }
        return out;
    }

    int produced = 0;
    int block = 0;
    while( produced < lines )
    {
        out += "구성 요소 " + std::to_string( block ) + "\n";
        out += "------------------\n\n";
        out += "이 절은 ``ISvcHost`` 와 **필수** 계약을 설명한다. :ref:`svc-contract` 을 함께 본다.\n\n";
        out += ".. code-block:: cpp\n";
        out += "   :linenos:\n";
        out += "   :caption: 예시\n\n";
        out += "   auto* p = host->Resolve< ISvcHost >();\n";
        out += "   if( p != nullptr && p->State() == Ready )\n";
        out += "       p->Start( a * b, c );\n\n";
        out += ":필드: 값 하나\n";
        out += ":다른 필드: ``literal`` 을 포함한다\n\n";
        out += ".. note::\n\n";
        out += "   `참고 링크`_ 와 |제품명| 치환을 쓴다.\n\n";
        produced += 18;
        ++block;
    }
    return out;
}

const char* corpusName( const Corpus kind )
{
    return kind == Corpus::Hangul ? "한글 산문" : "기술 문서";
}

/// MRST_PERF_CORPUS 로 실제 파일을 지정하면 그것도 함께 잰다. 없으면 합성만 돈다.
std::string realCorpus()
{
    const QByteArray path = qgetenv( "MRST_PERF_CORPUS" ).trimmed();
    if( path.isEmpty() )
        return {};

    QFile file( QString::fromLocal8Bit( path ) );
    if( !file.open( QIODevice::ReadOnly ) )
        return {};

    QByteArray raw = file.readAll();
    if( raw.startsWith( "\xEF\xBB\xBF" ) )
        raw.remove( 0, 3 );
    // 편집기는 적재할 때 개행을 LF 로 정규화한다 (TextFileSession::normalizeLineEndings).
    raw.replace( "\r\n", "\n" ).replace( '\r', '\n' );
    return std::string( raw.constData(), static_cast< std::size_t >( raw.size() ) );
}

std::vector< int > lineStarts( const std::string& text )
{
    std::vector< int > starts{ 0 };
    for( std::size_t i = 0; i < text.size(); ++i )
        if( text[ i ] == '\n' )
            starts.push_back( static_cast< int >( i ) + 1 );
    return starts;
}

QString megabytesPerSecond( const std::size_t bytes, const double ms )
{
    if( ms <= 0.0 )
        return QStringLiteral( "-" );
    return QString::number( ( static_cast< double >( bytes ) / ( 1024.0 * 1024.0 ) ) / ( ms / 1000.0 ),
                            'f', 1 );
}

}   // namespace

/// 현행 reST 렉서의 비용 구조를 잰다.
///
/// 강조 경로는 이미 화면 창 단위이므로(ScintillaQtDirectBackend::handleStyleNeeded)
/// 전체 문서 수치만으로는 체감 비용을 알 수 없다. 세 축을 나눠 잰다 —
/// 전체 문서 1회(파일 열기·전체 재칠), 화면 한 장(스크롤·타이핑),
/// 줄 종류별 단가(어느 분기가 비싼가).
///
/// **수치를 보는 방법.** Windows 에서 QTest 의 기본 로거는 stdout 이 리다이렉트되면
/// `qInfo()` 를 `OutputDebugString` 으로 보낸다. 그래서 셸에서 파이프로 받으면
/// 아무것도 보이지 않는다. IDE 의 출력 창에서 실행하거나, 파일 로거를 쓴다.
///
///     set MRST_PERF=1
///     mrst_tests.exe -o perf.txt,txt
///
/// 다만 러너(tests/main.cpp)가 등록된 클래스마다 qExec 를 부르고 각 호출이 같은
/// 파일을 새로 열므로, 한 번에 돌리면 **마지막 클래스의 로그만 남는다.**
/// 이 클래스만 보려면 IDE 에서 이 타깃의 단일 클래스를 실행한다.
class RstLexerPerfTest : public QObject
{
    Q_OBJECT

private slots:
    /// 전체 문서 1회. applyLanguage() 의 SCI_COLOURISE(0,-1) 과 접기 디바운스가 치르는 값.
    void fullDocumentCost();
    /// 화면 한 장(60줄) + 앞뒤 문맥 4줄. handleStyleNeeded() 한 번의 값.
    void windowedCost();
    /// 줄 종류별 tokenizeLine 단가. 어느 분기에 비용이 몰리는지 가른다.
    void lineClassCost();
};

void RstLexerPerfTest::fullDocumentCost()
{
    MRST_REQUIRE_PERF_BUILD();

    const RstContainerLexer lexer;
    static const int        sizes[] = { 2000, 20000, 200000 };

    qInfo().noquote() << QStringLiteral( "\n전체 문서 1회 (ms, 중앙값 %1회 / 최솟값)" ).arg( kRepeat );
    qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4 %5" )
                             .arg( QStringLiteral( "코퍼스" ), -12 )
                             .arg( QStringLiteral( "줄" ), 8 )
                             .arg( QStringLiteral( "KB" ), 8 )
                             .arg( QStringLiteral( "styleBytes" ), 22 )
                             .arg( QStringLiteral( "computeFoldLevels" ), 22 );

    for( const Corpus kind : { Corpus::Hangul, Corpus::Technical } )
    {
        for( const int lines : sizes )
        {
            const std::string text = buildCorpus( kind, lines );

            std::vector< double > style;
            std::vector< double > fold;
            for( int round = 0; round < kRepeat; ++round )
            {
                QElapsedTimer timer;
                timer.start();
                const std::vector< unsigned char > bytes = lexer.styleBytes( text );
                style.push_back( static_cast< double >( timer.nsecsElapsed() ) / 1e6 );
                QVERIFY( bytes.size() == text.size() );

                timer.restart();
                const std::vector< FoldLine > folds = computeFoldLevels( text );
                fold.push_back( static_cast< double >( timer.nsecsElapsed() ) / 1e6 );
                QVERIFY( !folds.empty() );
            }

            qInfo().noquote()
                << QStringLiteral( "%1 %2 %3 %4 %5" )
                       .arg( QString::fromUtf8( corpusName( kind ) ), -12 )
                       .arg( QString::number( lines ), 8 )
                       .arg( QString::number( text.size() / 1024 ), 8 )
                       .arg( QStringLiteral( "%1 / %2 (%3 MB/s)" )
                                 .arg( median( style ), 0, 'f', 2 )
                                 .arg( smallest( style ), 0, 'f', 2 )
                                 .arg( megabytesPerSecond( text.size(), median( style ) ) ),
                             22 )
                       .arg( QStringLiteral( "%1 / %2 (%3 MB/s)" )
                                 .arg( median( fold ), 0, 'f', 2 )
                                 .arg( smallest( fold ), 0, 'f', 2 )
                                 .arg( megabytesPerSecond( text.size(), median( fold ) ) ),
                             22 );
        }
    }

    const std::string real = realCorpus();
    if( !real.empty() )
    {
        std::vector< double > style;
        for( int round = 0; round < kRepeat; ++round )
        {
            QElapsedTimer timer;
            timer.start();
            g_sink = lexer.styleBytes( real ).size();
            style.push_back( static_cast< double >( timer.nsecsElapsed() ) / 1e6 );
        }
        qInfo().noquote() << QStringLiteral( "MRST_PERF_CORPUS  %1 KB  styleBytes %2 ms (%3 MB/s)" )
                                 .arg( real.size() / 1024 )
                                 .arg( median( style ), 0, 'f', 2 )
                                 .arg( megabytesPerSecond( real.size(), median( style ) ) );
    }
}

void RstLexerPerfTest::windowedCost()
{
    MRST_REQUIRE_PERF_BUILD();

    const RstContainerLexer lexer;
    constexpr int           kViewport = 60;   ///< 화면에 보이는 줄 수
    constexpr int           kContext = 4;     ///< handleStyleNeeded 가 앞뒤로 넓히는 줄 수
    constexpr int           kSamples = 50;    ///< 문서 전체에 고르게 흩뿌린 창의 개수

    qInfo().noquote() << QStringLiteral( "\n화면 한 장(%1줄) 렉싱 (창 1개 평균 ms)" ).arg( kViewport );

    for( const Corpus kind : { Corpus::Hangul, Corpus::Technical } )
    {
        const std::string        text = buildCorpus( kind, 20000 );
        const std::vector< int > starts = lineStarts( text );
        const int                lineCount = static_cast< int >( starts.size() );
        QVERIFY( lineCount > kViewport + 2 * kContext );

        std::vector< std::string > chunks;
        chunks.reserve( kSamples );
        for( int s = 0; s < kSamples; ++s )
        {
            const int first = kContext + ( lineCount - kViewport - 2 * kContext ) * s / kSamples;
            const int last = qMin( lineCount - 1, first + kViewport + kContext );
            const int begin = starts[ first - kContext ];
            const int end = ( last + 1 < lineCount ) ? starts[ last + 1 ]
                                                     : static_cast< int >( text.size() );
            chunks.push_back( text.substr( static_cast< std::size_t >( begin ),
                                           static_cast< std::size_t >( end - begin ) ) );
        }

        std::vector< double > samples;
        for( int round = 0; round < kRepeat; ++round )
        {
            QElapsedTimer timer;
            timer.start();
            for( const std::string& chunk : chunks )
                g_sink = lexer.styleBytes( chunk ).size();
            samples.push_back( static_cast< double >( timer.nsecsElapsed() ) / 1e6 / kSamples );
        }

        qInfo().noquote() << QStringLiteral( "%1  %2 ms (최솟값 %3 ms)" )
                                 .arg( QString::fromUtf8( corpusName( kind ) ), -12 )
                                 .arg( median( samples ), 0, 'f', 4 )
                                 .arg( smallest( samples ), 0, 'f', 4 );
    }
}

void RstLexerPerfTest::lineClassCost()
{
    MRST_REQUIRE_PERF_BUILD();

    const RstContainerLexer lexer;
    constexpr int           kIterations = 20000;

    struct Case
    {
        const char* label;
        const char* previous;
        const char* line;
        const char* next;
    };
    // 마지막 셋만 styleInline() 까지 내려가 정규식 6회 스윕을 치른다.
    // 나머지는 앞쪽 분기에서 조기 반환한다.
    static const Case cases[] = {
        { "빈 줄", "본문", "", "본문" },
        { "제목 글자", "", "프리다의 결심", "==============" },
        { "장식 줄", "프리다의 결심", "==============", "" },
        { "directive", "", ".. code-block:: cpp", "" },
        { "명시 마크업", "", ".. _target-name:", "" },
        { "필드", "", "   :caption: 예시", "" },
        { "한글 산문", "", "상업 길드에 계신 조부가 보낸 급사가 말을 타고 달려와 그렇게 말했다.", "" },
        { "영문 산문", "", "The service host resolves the interface and starts the worker thread.", "" },
        { "인라인 조밀", "", "이 절은 ``ISvcHost`` 와 **필수** 계약을 :ref:`svc` 로 설명한다.", "" },
    };

    qInfo().noquote() << QStringLiteral( "\n줄 종류별 tokenizeLine 단가 (ns/줄)" );
    qInfo().noquote() << QStringLiteral( "%1 %2 %3" )
                             .arg( QStringLiteral( "줄 종류" ), -14 )
                             .arg( QStringLiteral( "바이트" ), 8 )
                             .arg( QStringLiteral( "ns/줄" ), 12 );

    for( const Case& probe : cases )
    {
        const std::string previous( probe.previous );
        const std::string line( probe.line );
        const std::string next( probe.next );

        std::vector< double > samples;
        for( int round = 0; round < kRepeat; ++round )
        {
            QElapsedTimer timer;
            timer.start();
            std::size_t accumulated = 0;
            for( int i = 0; i < kIterations; ++i )
                accumulated += lexer.tokenizeLine( line, previous, next, {} ).size();
            samples.push_back( static_cast< double >( timer.nsecsElapsed() ) / kIterations );
            g_sink = accumulated;
        }

        qInfo().noquote() << QStringLiteral( "%1 %2 %3" )
                                 .arg( QString::fromUtf8( probe.label ), -14 )
                                 .arg( QString::number( line.size() ), 8 )
                                 .arg( QString::number( median( samples ), 'f', 0 ), 12 );
    }
}

MRST_REGISTER_TEST( RstLexerPerfTest );

#include "tst_RstLexerPerf.moc"
