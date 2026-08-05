#include "TestRunner.hpp"

#include "editor/RstContainerLexer.hpp"

#include <QTest>

#include <string>
#include <vector>

using namespace mrst::rst;

namespace {

/// 특정 바이트 위치의 스타일.
int styleAt( const RstContainerLexer& lexer, const std::string& text, std::size_t byteOffset )
{
    const std::vector< unsigned char > styles = lexer.styleBytes( text );
    return byteOffset < styles.size() ? static_cast< int >( styles[ byteOffset ] ) : -1;
}

}  // namespace

class TestRstContainerLexer : public QObject
{
    Q_OBJECT

private slots:
    /// 가장 중요한 불변식: 스타일 배열 길이는 항상 입력 바이트 수와 같아야 한다.
    /// 어긋나면 SCI_SETSTYLINGEX 가 문서를 밀어서 칠한다.
    void byteCountIsConserved_data();
    void byteCountIsConserved();

    void titleUnderlineMakesTitle();
    void standaloneTransition();
    void directiveIsUnknownUntilCachePopulated();
    void directiveBecomesValidOrInvalidAfterCache();
    void fieldNameSpansColons();
    void inlineMarkup();
    void commentLine();

    /// 비ASCII 앞선 인라인 마크업의 바이트 오프셋 정렬.
    /// src_cpp 포트는 regex 의 바이트 위치를 문자 인덱스로 오해해 여기서 밀렸다.
    void utf8OffsetsAreByteAccurate();
    void utf8OffsetsWithEmoji();

    void extractsDirectiveName_data();
    void extractsDirectiveName();
};

void TestRstContainerLexer::byteCountIsConserved_data()
{
    QTest::addColumn< QByteArray >( "text" );

    QTest::newRow( "empty" ) << QByteArray( "" );
    QTest::newRow( "single line" ) << QByteArray( "hello" );
    QTest::newRow( "trailing newline" ) << QByteArray( "hello\n" );
    QTest::newRow( "title" ) << QByteArray( "Title\n=====\n\nbody\n" );
    QTest::newRow( "directive" ) << QByteArray( ".. note::\n\n   text\n" );
    QTest::newRow( "field" ) << QByteArray( ":author: someone\n" );
    QTest::newRow( "inline" ) << QByteArray( "a **b** c *d* ``e`` |f| g_\n" );
    QTest::newRow( "korean" ) << QByteArray( "\xed\x95\x9c\xea\xb8\x80 **\xea\xb5\xb5\xea\xb2\x8c** \xeb\x81\x9d\n" );
    QTest::newRow( "crlf" ) << QByteArray( "Title\r\n=====\r\n\r\nbody\r\n" );
    QTest::newRow( "blank lines" ) << QByteArray( "\n\n\n" );
}

void TestRstContainerLexer::byteCountIsConserved()
{
    QFETCH( QByteArray, text );

    const RstContainerLexer lexer;
    const std::string input( text.constData(), static_cast< std::size_t >( text.size() ) );
    QCOMPARE( lexer.styleBytes( input ).size(), input.size() );
}

void TestRstContainerLexer::titleUnderlineMakesTitle()
{
    const RstContainerLexer lexer;
    const std::string text = "Title\n=====\nbody\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_TITLE ) );   // "Title"
    QCOMPARE( styleAt( lexer, text, 6 ), int( STYLE_TITLE ) );   // "=====" (제목의 밑줄)
    QCOMPARE( styleAt( lexer, text, 12 ), int( STYLE_DEFAULT ) ); // "body"
}

void TestRstContainerLexer::standaloneTransition()
{
    const RstContainerLexer lexer;
    // 앞 줄이 비어 있으면 제목 밑줄이 아니라 구분선이다.
    const std::string text = "\n----\n\n";
    QCOMPARE( styleAt( lexer, text, 1 ), int( STYLE_TRANSITION ) );
}

void TestRstContainerLexer::directiveIsUnknownUntilCachePopulated()
{
    const RstContainerLexer lexer;
    const std::string text = ".. note:: hi\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_EXPLICIT_MARKUP ) );  // ".. "
    QCOMPARE( styleAt( lexer, text, 3 ), int( STYLE_DIRECTIVE_UNKNOWN ) ); // "note"
    QCOMPARE( styleAt( lexer, text, 7 ), int( STYLE_DIRECTIVE_UNKNOWN ) ); // "::"
}

void TestRstContainerLexer::directiveBecomesValidOrInvalidAfterCache()
{
    RstMetadataCache cache;
    cache.updateFromCompletion( { { "note", "note::", "directive" } } );
    QVERIFY( cache.directivesPopulated );

    const RstContainerLexer lexer( cache );
    QCOMPARE( styleAt( lexer, ".. note:: hi\n", 3 ), int( STYLE_DIRECTIVE_VALID ) );
    QCOMPARE( styleAt( lexer, ".. bogus:: hi\n", 3 ), int( STYLE_DIRECTIVE_INVALID ) );
}

void TestRstContainerLexer::fieldNameSpansColons()
{
    const RstContainerLexer lexer;
    const std::string text = ":author: someone\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_FIELD_NAME ) );   // ':'
    QCOMPARE( styleAt( lexer, text, 7 ), int( STYLE_FIELD_NAME ) );   // 닫는 ':'
    QCOMPARE( styleAt( lexer, text, 9 ), int( STYLE_DEFAULT ) );      // "someone"
}

void TestRstContainerLexer::inlineMarkup()
{
    const RstContainerLexer lexer;
    //             0123456789...
    const std::string text = "a **b** c ``d``\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_DEFAULT ) );        // 'a'
    QCOMPARE( styleAt( lexer, text, 2 ), int( STYLE_STRONG ) );         // "**b**"
    QCOMPARE( styleAt( lexer, text, 6 ), int( STYLE_STRONG ) );
    QCOMPARE( styleAt( lexer, text, 8 ), int( STYLE_DEFAULT ) );        // 'c'
    QCOMPARE( styleAt( lexer, text, 10 ), int( STYLE_INLINE_LITERAL ) ); // "``d``"
}

void TestRstContainerLexer::commentLine()
{
    const RstContainerLexer lexer;
    // "::" 로 끝나는 directive 가 아닌 명시적 마크업은 주석이다.
    QCOMPARE( styleAt( lexer, ".. this is a comment\n", 0 ), int( STYLE_COMMENT ) );
}

void TestRstContainerLexer::utf8OffsetsAreByteAccurate()
{
    const RstContainerLexer lexer;
    // "한글 " = 7바이트 (3 + 3 + 공백 1), 그 뒤에 "**굵게**" 가 온다.
    const std::string text = "\xed\x95\x9c\xea\xb8\x80 **\xea\xb5\xb5\xea\xb2\x8c** end\n";

    // 바이트 배치: 한(0-2) 글(3-5) 공백(6) *(7) *(8) 굵(9-11) 게(12-14) *(15) *(16) 공백(17) e(18)
    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_DEFAULT ) );  // 한
    QCOMPARE( styleAt( lexer, text, 6 ), int( STYLE_DEFAULT ) );  // 공백
    QCOMPARE( styleAt( lexer, text, 7 ), int( STYLE_STRONG ) );   // 여는 '*'
    QCOMPARE( styleAt( lexer, text, 8 ), int( STYLE_STRONG ) );   // 여는 '*'
    QCOMPARE( styleAt( lexer, text, 14 ), int( STYLE_STRONG ) );  // 게 마지막 바이트
    QCOMPARE( styleAt( lexer, text, 15 ), int( STYLE_STRONG ) );  // 닫는 '*'
    QCOMPARE( styleAt( lexer, text, 16 ), int( STYLE_STRONG ) );  // 닫는 '*'
    QCOMPARE( styleAt( lexer, text, 17 ), int( STYLE_DEFAULT ) ); // 공백
    QCOMPARE( styleAt( lexer, text, 18 ), int( STYLE_DEFAULT ) ); // 'e'
}

void TestRstContainerLexer::utf8OffsetsWithEmoji()
{
    const RstContainerLexer lexer;
    // U+1F600 = 4바이트. 그 뒤 "``code``".
    const std::string text = "\xf0\x9f\x98\x80 ``code``\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_DEFAULT ) );
    QCOMPARE( styleAt( lexer, text, 4 ), int( STYLE_DEFAULT ) );        // 공백
    QCOMPARE( styleAt( lexer, text, 5 ), int( STYLE_INLINE_LITERAL ) ); // '`'
    QCOMPARE( styleAt( lexer, text, 12 ), int( STYLE_INLINE_LITERAL ) ); // 닫는 '`'
}

void TestRstContainerLexer::extractsDirectiveName_data()
{
    QTest::addColumn< QString >( "label" );
    QTest::addColumn< QString >( "insertText" );
    QTest::addColumn< QString >( "expected" );

    QTest::newRow( "plain" ) << "note" << "note::" << "note";
    QTest::newRow( "with dots" ) << "note" << ".. note::" << "note";
    QTest::newRow( "snippet" ) << "image" << "image:: ${1:path}" << "image";
    QTest::newRow( "label fallback" ) << ".. code-block::" << "" << "code-block";
    QTest::newRow( "domain" ) << "py:function" << "py:function::" << "py:function";
}

void TestRstContainerLexer::extractsDirectiveName()
{
    QFETCH( QString, label );
    QFETCH( QString, insertText );
    QFETCH( QString, expected );

    const std::string actual = extractDirectiveName( label.toStdString(), insertText.toStdString() );
    QCOMPARE( QString::fromStdString( actual ), expected );
}

MRST_REGISTER_TEST( TestRstContainerLexer );

#include "tst_RstContainerLexer.moc"
