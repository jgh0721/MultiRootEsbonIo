#include "TestRunner.hpp"

#include "core/solRestOutlineService.hpp"
#include "editor/RstStructure.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace mrst;

namespace {

QString heading( const QString& title, const QChar adornment, const bool overline = false )
{
    // 장식 줄은 제목의 **표시 폭** 이상이어야 한다. docutils 는 column_width() 로
    // 견주므로 한글 한 글자(두 칸)짜리 제목에는 두 자가 필요하다 — 예전 헬퍼는
    // QString::length() 로 재서 "첫째" 에 세 자면 된다고 보았고, 그것이 곧
    // 개요만 제목으로 잡고 화면에는 색이 없던 원인이었다.
    const QByteArray titleUtf8 = title.toUtf8();
    const auto       width = static_cast< qsizetype >( mrst::rst::columnWidth(
        std::string_view( titleUtf8.constData(), static_cast< std::size_t >( titleUtf8.size() ) ) ) );
    const QString    rule = QString( qMax( width, qsizetype( 3 ) ), adornment );
    return overline ? QStringLiteral( "%1\n%2\n%1\n" ).arg( rule, title )
                    : QStringLiteral( "%1\n%2\n" ).arg( title, rule );
}

}  // namespace

/// Esbonio 가 데워지기 전까지 개요 패널을 채우는 것이 이 파서의 전부다.
/// 계층을 잘못 잡으면 트리가 통째로 어긋나므로 거기에 집중한다.
class TestRstOutline : public QObject
{
    Q_OBJECT

private slots:
    void emptyTextHasNoSymbols();
    void findsUnderlineHeadings();
    /// docutils 는 밑줄 문자의 의미를 문서 안에서 **처음 나온 순서** 로 정한다.
    /// 고정 표(= 는 1단계, - 는 2단계 …)를 쓰면 문서마다 틀린다.
    void levelsFollowFirstSeenOrder();
    void handlesOverlineHeadings();
    void siblingsAtSameLevelStayFlat();
    void ignoresTooShortUnderline();
    void ignoresBulletListsAndTransitions();
    void reportsOneBasedTitleLine();
    void collectsProjectDocumentsRootDocFirst();
    void collectsProjectDocumentsSkipsBuildDirs();
    void collectsProjectDocumentsReportsTruncation();
};

void TestRstOutline::emptyTextHasNoSymbols()
{
    QVERIFY( parseRstOutline( QString{} ).isEmpty() );
    QVERIFY( parseRstOutline( QStringLiteral( "제목 없는 그냥 문단입니다.\n" ) ).isEmpty() );
}

void TestRstOutline::findsUnderlineHeadings()
{
    const QString text = heading( QStringLiteral( "첫째" ), QLatin1Char( '=' ) )
                       + QStringLiteral( "\n본문\n\n" )
                       + heading( QStringLiteral( "둘째" ), QLatin1Char( '=' ) );

    const QVector< OutlineSymbol > symbols = parseRstOutline( text, QStringLiteral( "C:/a.rst" ) );
    QCOMPARE( symbols.size(), 2 );
    QCOMPARE( symbols.at( 0 ).name, QStringLiteral( "첫째" ) );
    QCOMPARE( symbols.at( 1 ).name, QStringLiteral( "둘째" ) );
    QCOMPARE( symbols.at( 0 ).path, QStringLiteral( "C:/a.rst" ) );
    QVERIFY( symbols.at( 0 ).children.isEmpty() );
}

void TestRstOutline::levelsFollowFirstSeenOrder()
{
    // '~' 가 먼저 나왔으므로 1단계, '=' 가 그 다음이라 2단계다.
    // 관습적인 순서와 반대인데, docutils 도 그렇게 읽는다.
    const QString text = heading( QStringLiteral( "루트" ), QLatin1Char( '~' ) )
                       + heading( QStringLiteral( "하위" ), QLatin1Char( '=' ) );

    const QVector< OutlineSymbol > symbols = parseRstOutline( text );
    QCOMPARE( symbols.size(), 1 );
    QCOMPARE( symbols.at( 0 ).name, QStringLiteral( "루트" ) );
    QCOMPARE( symbols.at( 0 ).children.size(), 1 );
    QCOMPARE( symbols.at( 0 ).children.at( 0 ).name, QStringLiteral( "하위" ) );
}

void TestRstOutline::handlesOverlineHeadings()
{
    const QString text = heading( QStringLiteral( "책 제목" ), QLatin1Char( '=' ), true )
                       + heading( QStringLiteral( "장" ), QLatin1Char( '=' ) );

    const QVector< OutlineSymbol > symbols = parseRstOutline( text );
    QCOMPARE( symbols.size(), 1 );
    QCOMPARE( symbols.at( 0 ).name, QStringLiteral( "책 제목" ) );
    // 윗줄 있는 '=' 와 밑줄만 있는 '=' 는 서로 다른 단계다.
    QCOMPARE( symbols.at( 0 ).children.size(), 1 );
    QCOMPARE( symbols.at( 0 ).children.at( 0 ).name, QStringLiteral( "장" ) );
}

void TestRstOutline::siblingsAtSameLevelStayFlat()
{
    const QString text = heading( QStringLiteral( "A" ), QLatin1Char( '=' ) )
                       + heading( QStringLiteral( "A1" ), QLatin1Char( '-' ) )
                       + heading( QStringLiteral( "A2" ), QLatin1Char( '-' ) )
                       + heading( QStringLiteral( "B" ), QLatin1Char( '=' ) )
                       + heading( QStringLiteral( "B1" ), QLatin1Char( '-' ) );

    const QVector< OutlineSymbol > symbols = parseRstOutline( text );
    QCOMPARE( symbols.size(), 2 );
    QCOMPARE( symbols.at( 0 ).children.size(), 2 );
    QCOMPARE( symbols.at( 0 ).children.at( 1 ).name, QStringLiteral( "A2" ) );
    QCOMPARE( symbols.at( 1 ).children.size(), 1 );
    QCOMPARE( symbols.at( 1 ).children.at( 0 ).name, QStringLiteral( "B1" ) );
}

void TestRstOutline::ignoresTooShortUnderline()
{
    // 밑줄이 제목보다 짧으면 docutils 는 섹션으로 보지 않는다.
    QVERIFY( parseRstOutline( QStringLiteral( "긴 제목입니다\n==\n" ) ).isEmpty() );
}

void TestRstOutline::ignoresBulletListsAndTransitions()
{
    const QString text = QStringLiteral( "- 항목 하나\n- 항목 둘\n\n----\n\n본문\n" );
    QVERIFY2( parseRstOutline( text ).isEmpty(), "목록/구분선을 제목으로 잡으면 안 된다" );
}

void TestRstOutline::reportsOneBasedTitleLine()
{
    //  1: (빈 줄)
    //  2: 제목
    //  3: =====
    const QVector< OutlineSymbol > symbols =
        parseRstOutline( QStringLiteral( "\n제목\n=====\n" ) );
    QCOMPARE( symbols.size(), 1 );
    QCOMPARE( symbols.at( 0 ).line, 2 );
}

void TestRstOutline::collectsProjectDocumentsRootDocFirst()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    const QDir root( temp.path() );

    for( const QString& name : { QStringLiteral( "aaa.rst" ), QStringLiteral( "index.rst" ),
                                QStringLiteral( "zzz.rst" ) } )
    {
        QFile file( root.filePath( name ) );
        QVERIFY( file.open( QIODevice::WriteOnly ) );
        file.write( "T\n=\n" );
    }

    int total = 0;
    const QStringList documents =
        collectProjectDocuments( temp.path(), QStringLiteral( "index" ), 0, &total );

    QCOMPARE( total, 3 );
    QCOMPARE( documents.size(), 3 );
    QCOMPARE( QFileInfo( documents.first() ).fileName(), QStringLiteral( "index.rst" ) );
    QCOMPARE( QFileInfo( documents.at( 1 ) ).fileName(), QStringLiteral( "aaa.rst" ) );
}

void TestRstOutline::collectsProjectDocumentsSkipsBuildDirs()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    const QDir root( temp.path() );
    QVERIFY( root.mkpath( QStringLiteral( "_build/html" ) ) );
    QVERIFY( root.mkpath( QStringLiteral( "guide" ) ) );

    for( const QString& relative : { QStringLiteral( "index.rst" ),
                                    QStringLiteral( "guide/intro.rst" ),
                                    QStringLiteral( "_build/html/leftover.rst" ) } )
    {
        QFile file( root.filePath( relative ) );
        QVERIFY( file.open( QIODevice::WriteOnly ) );
        file.write( "T\n=\n" );
    }

    const QStringList documents = collectProjectDocuments( temp.path(), QStringLiteral( "index" ), 0 );
    QCOMPARE( documents.size(), 2 );
    for( const QString& path : documents )
        QVERIFY2( !path.contains( QStringLiteral( "_build" ) ), qPrintable( path ) );
}

void TestRstOutline::collectsProjectDocumentsReportsTruncation()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    const QDir root( temp.path() );

    for( int index = 0; index < 10; ++index )
    {
        QFile file( root.filePath( QStringLiteral( "doc%1.rst" ).arg( index, 2, 10, QLatin1Char( '0' ) ) ) );
        QVERIFY( file.open( QIODevice::WriteOnly ) );
        file.write( "T\n=\n" );
    }

    int total = 0;
    const QStringList documents =
        collectProjectDocuments( temp.path(), QStringLiteral( "index" ), 4, &total );

    // 자른 사실이 밖으로 드러나야 한다. 조용히 자르면 "전부 보여 준 것" 처럼 읽힌다.
    QCOMPARE( documents.size(), 4 );
    QCOMPARE( total, 10 );
}

MRST_REGISTER_TEST( TestRstOutline );

#include "tst_RstOutline.moc"
