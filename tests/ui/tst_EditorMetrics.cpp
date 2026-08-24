#include "TestRunner.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTest>

#include <ScintillaEditBase.h>
#include <Scintilla.h>

#include "editor/Utf16Length.hpp"

#include <algorithm>
#include <vector>

/// 상태바의 "문자 N" 을 문서 복사 없이 유지하는 경로의 안전장치.
///
/// 예전 구현은 부를 때마다 `text().size()` 였다 — SCI_GETTEXT 로 문서를 복사하고
/// QString::fromUtf8 로 디코딩해 **글자 수 하나를 얻으려고** 문서 크기의 memcpy 와
/// 그 2배의 힙 할당을 치렀다. 그리고 그 경로는 캐럿을 움직일 때마다 돌았다.
///
/// 지금은 값을 캐시하고 **편집분만 더하고 뺀다.** 그래서 검증할 것이 둘이다 —
/// (1) `utf16Length()` 가 `QString::fromUtf8().size()` 와 같은 값을 내는가,
/// (2) 그 증분 유지가 전량 계산과 어긋나지 않는가. (2) 가 이 파일의 핵심이다.
/// 어긋나면 상태바 숫자가 조용히 틀리고, 눈으로는 알아채기 어렵다.
class EditorMetricsTest : public QObject
{
    Q_OBJECT

private slots:
    void                                utf16LengthMatchesQString_data();
    void                                utf16LengthMatchesQString();
    void                                incrementalCountSurvivesEditing();
    void                                selectionLengthMatchesQString();
    void                                getSelTextExcludesTerminator();
    void                                fullRecountCost();
};

namespace {

/// 코드페이지와 마진만 맞춘 맨 편집기. 우리 래퍼(ScintillaQtDirectBackend) 를
/// 쓰지 않는 이유는 tst_ScintillaTextMetrics 와 같다 — 재려는 성질이 Scintilla 의
/// 것이고, 래퍼를 끌어오면 렉서 등록부와 테마 관리자까지 딸려온다. 대신 백엔드가
/// 쓰는 것과 **같은 메시지 조합과 같은 헬퍼**를 여기서 재현한다.
void configure( ScintillaEditBase& editor )
{
    editor.send( SCI_SETCODEPAGE, SC_CP_UTF8 );
    editor.send( SCI_SETMARGINWIDTHN, 0, 0 );
    editor.send( SCI_SETMARGINWIDTHN, 1, 0 );
    editor.send( SCI_SETMARGINWIDTHN, 2, 0 );
}

/// 기준값. 예전 구현이 내던 바로 그 숫자다.
int countByCopy( ScintillaEditBase& editor )
{
    const sptr_t length = editor.send( SCI_GETTEXTLENGTH );
    if( length <= 0 )
        return 0;
    QByteArray buffer( static_cast< int >( length ) + 1, Qt::Uninitialized );
    editor.send( SCI_GETTEXT, length + 1, reinterpret_cast< sptr_t >( buffer.data() ) );
    buffer.truncate( static_cast< int >( length ) );
    return static_cast< int >( QString::fromUtf8( buffer ).size() );
}

/// ScintillaQtDirectBackend::recountCharacters() 와 같은 방식 — 갭 버퍼를 조각별로
/// 직접 훑고 복사하지 않는다.
int recountViaRangePointer( ScintillaEditBase& editor )
{
    const sptr_t length = editor.send( SCI_GETTEXTLENGTH );
    if( length <= 0 )
        return 0;

    const auto chunk = [ &editor ]( sptr_t from, sptr_t count ) -> int {
        if( count <= 0 )
            return 0;
        const auto* data = reinterpret_cast< const char* >(
            editor.send( SCI_GETRANGEPOINTER, from, count ) );
        if( data == nullptr )
            return 0;
        return mrst::sci::utf16Length( QByteArrayView( data, static_cast< qsizetype >( count ) ) );
    };

    const sptr_t gap = editor.send( SCI_GETGAPPOSITION );
    if( gap <= 0 || gap >= length )
        return chunk( 0, length );
    return chunk( 0, gap ) + chunk( gap, length - gap );
}

/// 백엔드의 selectionBytes() 와 같다. 종료문자를 잘라낸 선택 바이트.
QByteArray selectionBytes( ScintillaEditBase& editor )
{
    const sptr_t length = editor.send( SCI_GETSELTEXT );
    if( length <= 0 )
        return {};
    QByteArray buffer( static_cast< int >( length ) + 1, Qt::Uninitialized );
    editor.send( SCI_GETSELTEXT, 0, reinterpret_cast< sptr_t >( buffer.data() ) );
    buffer.truncate( static_cast< int >( length ) );
    return buffer;
}

const char* const kEmoji = "\xF0\x9F\x98\x80";                       // U+1F600, 서러게이트 쌍
const char* const kHangul = "\xEA\xB0\x80\xEB\x82\x98\xEB\x8B\xA4";  // 가나다
const char* const kSymbols = "\xE2\x80\xBB \xE2\x84\x83";            // ※ ℃

}   // namespace

void EditorMetricsTest::utf16LengthMatchesQString_data()
{
    QTest::addColumn< QByteArray >( "utf8" );

    QTest::newRow( "empty" ) << QByteArray();
    QTest::newRow( "ascii" ) << QByteArray( "The service host resolves the interface.\n" );
    QTest::newRow( "hangul" ) << QByteArray( kHangul );
    QTest::newRow( "symbols" ) << QByteArray( kSymbols );
    // 핵심 사례. U+1F600 은 UTF-16 에서 2 코드 유닛이지만 코드포인트로는 1 이다.
    // 서러게이트 쌍을 1 로 세면 여기서 갈린다.
    QTest::newRow( "emoji" ) << QByteArray( kEmoji );
    QTest::newRow( "emoji-with-modifier" )
        << QByteArray( "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB" );   // 👍 + 피부톤
    QTest::newRow( "mixed-rest-markup" )
        << QByteArray( "\xEC\x9D\xB4 \xEC\xA0\x88\xEC\x9D\x80 ``ISvcHost`` \xEC\x99\x80 "
                       "**\xED\x95\x84\xEC\x88\x98** :ref:`svc-contract`\n" );
    QTest::newRow( "multiline-with-blank" )
        << QByteArray( "first\nsecond\nthird\n\nfifth\n" );
    QTest::newRow( "everything" )
        << ( QByteArray( "a " ) + kHangul + " b " + kEmoji + " c " + kSymbols + "\n" );
}

void EditorMetricsTest::utf16LengthMatchesQString()
{
    QFETCH( QByteArray, utf8 );

    QCOMPARE( mrst::sci::utf16Length( utf8 ),
              static_cast< int >( QString::fromUtf8( utf8 ).size() ) );
}

void EditorMetricsTest::incrementalCountSurvivesEditing()
{
    ScintillaEditBase editor;
    configure( editor );

    // 백엔드가 하는 것과 같은 계약으로 캐시를 유지한다 — SCI_SETMODEVENTMASK 로
    // 좁힌 통지에서 changedText 의 UTF-16 길이만큼 더하고 뺀다.
    editor.send( SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT );

    int cached = 0;
    connect( &editor, &ScintillaEditBase::modified, this,
            [ &cached ]( Scintilla::ModificationFlags type, Scintilla::Position,
                        Scintilla::Position, Scintilla::Position,
                        const QByteArray& changedText, Scintilla::Position,
                        Scintilla::FoldLevel, Scintilla::FoldLevel ) {
                const bool inserted = ( static_cast< int >( type ) & SC_MOD_INSERTTEXT ) != 0;
                const bool deleted = ( static_cast< int >( type ) & SC_MOD_DELETETEXT ) != 0;
                if( !inserted && !deleted )
                    return;
                const int delta = mrst::sci::utf16Length( changedText );
                cached += inserted ? delta : -delta;
            } );

    /// 문자 단위 오프셋을 바이트 위치로 옮긴다.
    ///
    /// **삭제는 반드시 문자 경계여야 한다.** 다중 바이트 문자를 쪼개면 문서에
    /// 올바르지 않은 UTF-8 이 남는데, 그때 `QString::fromUtf8` 은 깨진 바이트를
    /// U+FFFD 하나로 세고 `utf16Length` 는 0 으로 세므로 두 값이 정당하게 갈린다
    /// (Utf16Length.hpp 의 가정). 편집기에서는 Scintilla 가 캐럿과 선택을 문자
    /// 경계로 붙잡아 두므로 그런 문서가 생기지 않는다. 이 테스트도 그 계약을 지킨다.
    const auto byteAt = [ &editor ]( int charOffset ) -> sptr_t {
        return editor.send( SCI_POSITIONRELATIVE, 0, charOffset );
    };

    struct Step
    {
        const char* label;
        bool        insert;   ///< false 면 삭제
        const char* text;     ///< 삽입할 바이트 (삭제면 무시)
        int         at;       ///< 삽입/삭제 시작 — **문자 단위**
        int         chars;    ///< 삭제 길이 — **문자 단위** (삽입이면 무시)
    };

    // 삽입·삭제·되돌리기를 섞고, 서러게이트 쌍이 통째로 오가는 경우와 여러
    // 코드포인트가 한 번에 지워지는 경우를 모두 지난다.
    const Step steps[] = {
        { "ascii 삽입",        true,  "hello world\n", 0,  0 },
        { "한글 삽입",         true,  kHangul,         5,  0 },
        { "이모지 삽입",       true,  kEmoji,          0,  0 },
        { "기호 삽입",         true,  kSymbols,        4,  0 },
        { "이모지 삭제",       false, nullptr,         0,  1 },
        { "한글 한 글자 삭제", false, nullptr,         6,  1 },
        { "여러 글자 삭제",    false, nullptr,         0,  5 },
        { "다시 삽입",         true,  "TAIL",          0,  0 },
    };

    QStringList problems;
    for( const Step& step : steps )
    {
        if( step.insert )
        {
            const sptr_t at = byteAt( step.at );
            editor.send( SCI_SETTARGETRANGE, at, at );
            editor.sends( SCI_REPLACETARGET, qstrlen( step.text ), step.text );
        }
        else
        {
            const sptr_t from = byteAt( step.at );
            const sptr_t to = byteAt( step.at + step.chars );
            editor.send( SCI_DELETERANGE, from, to - from );
        }

        const int truth = countByCopy( editor );
        if( cached != truth )
        {
            problems << QStringLiteral( "%1: 증분=%2 전량=%3" )
                            .arg( QString::fromUtf8( step.label ) ).arg( cached ).arg( truth );
        }
        // 갭 버퍼를 직접 훑는 전량 계산도 같은 값이어야 한다.
        if( recountViaRangePointer( editor ) != truth )
        {
            problems << QStringLiteral( "%1: RangePointer=%2 전량=%3" )
                            .arg( QString::fromUtf8( step.label ) )
                            .arg( recountViaRangePointer( editor ) ).arg( truth );
        }
    }

    // 되돌리기·다시하기도 같은 통지 경로를 타므로 캐시가 따라와야 한다.
    for( int i = 0; i < 4; ++i )
    {
        editor.send( SCI_UNDO );
        if( cached != countByCopy( editor ) )
            problems << QStringLiteral( "undo %1: 증분=%2 전량=%3" )
                            .arg( i ).arg( cached ).arg( countByCopy( editor ) );
    }
    for( int i = 0; i < 4; ++i )
    {
        editor.send( SCI_REDO );
        if( cached != countByCopy( editor ) )
            problems << QStringLiteral( "redo %1: 증분=%2 전량=%3" )
                            .arg( i ).arg( cached ).arg( countByCopy( editor ) );
    }

    QVERIFY2( problems.isEmpty(), qPrintable( problems.join( QStringLiteral( " | " ) ) ) );
}

void EditorMetricsTest::selectionLengthMatchesQString()
{
    // 선택은 **문자 경계**로만 움직인다. 바이트 오프셋을 하나씩 훑으면 다중
    // 바이트 문자를 쪼개는 범위가 만들어지는데, 그것은 편집기에서 생길 수 없는
    // 상태이고 Scintilla 와 QString 이 각각 다르게 복구하므로 비교할 값이 아니다.
    const QByteArray utf8 = QByteArray( "ab " ) + kEmoji + " " + kHangul + " cd\n";

    ScintillaEditBase editor;
    configure( editor );
    editor.sends( SCI_SETTEXT, 0, utf8.constData() );

    const sptr_t length = editor.send( SCI_GETTEXTLENGTH );
    QVERIFY( length > 0 );

    // 문자 경계 목록을 SCI_POSITIONRELATIVE 로 만든다.
    std::vector< sptr_t > boundaries;
    for( sptr_t pos = 0; pos <= length; )
    {
        boundaries.push_back( pos );
        if( pos == length )
            break;
        const sptr_t next = editor.send( SCI_POSITIONRELATIVE, pos, 1 );
        QVERIFY( next > pos );
        pos = next;
    }
    QVERIFY( boundaries.size() > 5 );

    QStringList problems;
    for( const sptr_t start : boundaries )
    {
        for( const sptr_t end : boundaries )
        {
            if( end < start )
                continue;
            editor.send( SCI_SETSEL, start, end );

            const QByteArray bytes = selectionBytes( editor );
            const int        viaHelper = mrst::sci::utf16Length( bytes );
            const int        viaQString = static_cast< int >( QString::fromUtf8( bytes ).size() );
            if( viaHelper != viaQString )
            {
                problems << QStringLiteral( "[%1,%2) helper=%3 QString=%4" )
                                .arg( start ).arg( end ).arg( viaHelper ).arg( viaQString );
            }
        }
    }

    QVERIFY2( problems.isEmpty(), qPrintable( problems.join( QStringLiteral( ", " ) ) ) );
}

void EditorMetricsTest::getSelTextExcludesTerminator()
{
    // 우리가 의존하는 상류 계약을 못 박는다. Scintilla 5.2 에서 SCI_GETSELTEXT 의
    // 반환값이 "종료문자 포함" 에서 "제외" 로 바뀌었고, 쓸 때는 그 길이 + NUL 한
    // 바이트를 쓴다. 예전 구현이 이것을 거꾸로 읽어 한 글자 선택이 빈 문자열로
    // 나왔고 버퍼 크기도 한 바이트 부족했다. 상류가 다시 바뀌면 여기가 먼저 깨진다.
    ScintillaEditBase editor;
    configure( editor );
    editor.sends( SCI_SETTEXT, 0, "abcdef" );

    editor.send( SCI_SETSEL, 2, 3 );   // 한 바이트 선택
    QCOMPARE( static_cast< int >( editor.send( SCI_GETSELTEXT ) ), 1 );
    QCOMPARE( selectionBytes( editor ), QByteArray( "c" ) );

    editor.send( SCI_SETSEL, 0, 6 );
    QCOMPARE( static_cast< int >( editor.send( SCI_GETSELTEXT ) ), 6 );
    QCOMPARE( selectionBytes( editor ), QByteArray( "abcdef" ) );
}

void EditorMetricsTest::fullRecountCost()
{
    if( qgetenv( "MRST_PERF" ).trimmed().isEmpty() )
        QSKIP( "성능 계측은 MRST_PERF=1 일 때만 돈다 (벽시계 값이라 CI 에서 단언하지 않는다)" );

    QByteArray text;
    text.reserve( 700 * 1024 );
    while( text.size() < 700 * 1024 )
    {
        text += "\xEC\x83\x81\xEC\x97\x85 \xEA\xB8\xB8\xEB\x93\x9C\xEC\x97\x90 ``ISvcHost`` "
                "\xEA\xB3\x84\xEC\x95\xBD\xEC\x9D\x84 \xEC\x84\xA4\xEB\xAA\x85\xED\x95\x9C\xEB\x8B\xA4.\n";
    }

    ScintillaEditBase editor;
    configure( editor );
    editor.sends( SCI_SETTEXT, 0, text.constData() );
    QCOMPARE( recountViaRangePointer( editor ), countByCopy( editor ) );

    constexpr int         kRepeat = 20;
    std::vector< double > rangeMs;
    std::vector< double > copyMs;
    std::vector< double > sciMs;
    int                   sink = 0;
    const sptr_t          length = editor.send( SCI_GETTEXTLENGTH );
    for( int round = 0; round < kRepeat; ++round )
    {
        QElapsedTimer timer;
        timer.start();
        sink += recountViaRangePointer( editor );
        rangeMs.push_back( static_cast< double >( timer.nsecsElapsed() ) / 1e6 );

        timer.restart();
        sink += countByCopy( editor );
        copyMs.push_back( static_cast< double >( timer.nsecsElapsed() ) / 1e6 );

        timer.restart();
        sink += static_cast< int >( editor.send( SCI_COUNTCODEUNITS, 0, length ) );
        sciMs.push_back( static_cast< double >( timer.nsecsElapsed() ) / 1e6 );
    }
    QVERIFY( sink != 0 );

    const auto smallest = []( const std::vector< double >& v ) {
        return v.empty() ? 0.0 : *std::min_element( v.begin(), v.end() );
    };
    // 증분 유지가 있으므로 이 값은 문서를 열 때 한 번만 든다. 그래도 세 방법의
    // 차이를 남겨 둔다 — SCI_COUNTCODEUNITS 가 가장 그럴듯해 보이는데 실제로는
    // 가장 느리다는 것이 이 작업에서 전제를 뒤집은 계측이었다.
    qInfo().noquote() << QStringLiteral( "%1 KB, 전량 1회 (최솟값) — RangePointer %2 ms / "
                                         "text() 복사 %3 ms / SCI_COUNTCODEUNITS %4 ms" )
                             .arg( text.size() / 1024 )
                             .arg( smallest( rangeMs ), 0, 'f', 4 )
                             .arg( smallest( copyMs ), 0, 'f', 4 )
                             .arg( smallest( sciMs ), 0, 'f', 4 );
}

MRST_REGISTER_TEST( EditorMetricsTest );

#include "tst_EditorMetrics.moc"
