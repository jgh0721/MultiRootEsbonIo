#include "TestRunner.hpp"

#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QStringList>
#include <QTest>

#include <ScintillaEditBase.h>
#include <Scintilla.h>

#include <algorithm>
#include <memory>
#include <vector>

/// 문서를 편집기에 밀어 넣는 비용(`ScintillaQtDirectBackend::setText` 상당)을 재는
/// 계측 하네스.
///
/// 재려는 것은 세 가지다.
///   1. `SCI_SETUNDOCOLLECTION` 을 끄고 넣으면 얼마나 줄어드는가.
///      Scintilla 자신이 `CellBuffer::InsertString` 에 "This takes up about half
///      load time" 이라고 적어 둔 경로다 — undo 스택에 문서 사본을 만든다.
///      우리는 넣은 직후 `SCI_EMPTYUNDOBUFFER` 로 그 사본을 버리므로 순수 낭비다.
///   2. `SCI_ALLOCATE` / `SCI_ALLOCATELINES` 로 미리 예약하면 얼마나 줄어드는가.
///      `SplitVector::RoomFor` 의 성장분은 `body.size()/6` 까지만 배로 늘어나
///      1.17 배씩 자란다. 라인 인덱스는 128 줄씩 나뉘어 들어오므로(CellBuffer.cxx
///      의 `PositionBlockSize`) 재할당이 여러 번 일어난다.
///   3. `SCI_SETTEXT` 대신 길이를 명시하는 `SCI_APPENDTEXT` 를 쓰면 어떤가.
///      `SCI_SETTEXT` 은 `strlen()` 으로 길이를 다시 재므로 문서 크기만큼 한 번
///      더 훑고, 본문에 NUL 이 있으면 거기서 잘린다.
///
/// **회차마다 편집기를 새로 만든다.** `SplitVector::DeleteAll` 은 용량을 줄이지
/// 않으므로(`DeleteRange` 만 부른다) 같은 문서에 두 번째로 넣으면 재할당이 아예
/// 일어나지 않아 2번의 효과가 사라진다. 실제로 파일을 여는 경로도 새 탭 =
/// 새 `ScintillaEditBase` = 빈 문서다.
///
/// 벽시계 값이라 CI 에서 단언하지 않는다 — `MRST_PERF` 를 준 사람만 돌린다.
class ScintillaDocumentLoadTest : public QObject
{
    Q_OBJECT

private slots:
    void                                insertCost();
    void                                foldLevelInjectionCost();
    void                                loadedDocumentsAgree();
};

namespace {

constexpr int kRepeat     = 7;
constexpr int kViewWidth  = 900;
constexpr int kViewHeight = 600;

/// 실제 한국어 기술문서에 가까운 말뭉치. 한글·ASCII·기호가 섞여야 라인 인덱스와
/// UTF-8 검사 경로가 모두 걸린다.
QByteArray buildCorpus( const int lines )
{
    static const char* tpl[] = {
        "이 문단은 문서 적재 비용을 재기 위한 한국어 산문이다. 세그먼트가 길어진다.",
        "``ScintillaQtDirectBackend::setText`` 는 SCI_SETTEXT 한 번으로 본문을 넣는다.",
        ".. note:: The undo history keeps a copy of every inserted byte by default.",
        "※ 주의: ○ 입력 범위는 −40 ℃ ~ +85 ℃ 이며 오차는 ±0.5 ℃ 이내여야 한다.",
        "   * :ref:`text-view-settings` -- see also :doc:`../guide/editor`",
        "+------------------+---------------------+------------------------+",
    };

    QByteArray text;
    text.reserve( static_cast< qsizetype >( lines ) * 128 );
    for( int i = 0; i < lines; ++i )
    {
        text += tpl[ i % 6 ];
        text += QByteArray( " (" ) + QByteArray::number( i ) + ')';
        if( i + 1 < lines )
            text += '\n';
    }
    return text;
}

enum class Knob
{
    Baseline,        ///< 지금 코드 그대로
    NoUndo,          ///< + SCI_SETUNDOCOLLECTION 0/1
    Allocate,        ///< + SCI_ALLOCATE, SCI_ALLOCATELINES
    NoUndoAllocate,  ///< 둘 다
    AppendText,      ///< 위에 더해 SCI_SETTEXT -> SCI_CLEARALL + SCI_APPENDTEXT
};

const char* knobName( const Knob knob )
{
    switch( knob )
    {
        case Knob::Baseline:       return "현재";
        case Knob::NoUndo:         return "+undo끔";
        case Knob::Allocate:       return "+예약";
        case Knob::NoUndoAllocate: return "+undo끔+예약";
        case Knob::AppendText:     return "+append";
    }
    return "?";
}

/// `ScintillaQtDirectBackend::setText` 와 같은 순서로 본문을 넣는다.
/// knob 이 요구하는 것만 더한다.
void pushText( ScintillaEditBase& editor, const QByteArray& utf8, const Knob knob )
{
    const bool noUndo = knob == Knob::NoUndo || knob == Knob::NoUndoAllocate
        || knob == Knob::AppendText;
    const bool reserve = knob == Knob::Allocate || knob == Knob::NoUndoAllocate
        || knob == Knob::AppendText;

    const int changeHistoryFlags = static_cast< int >( editor.send( SCI_GETCHANGEHISTORY ) );
    if( changeHistoryFlags != SC_CHANGE_HISTORY_DISABLED )
        editor.send( SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED );

    if( noUndo )
        editor.send( SCI_SETUNDOCOLLECTION, 0 );
    if( reserve )
    {
        // 줄 수를 세는 비용도 측정 안에 둔다 — 실제로 치러야 하는 값이다.
        const qsizetype newlines = std::count( utf8.constBegin(), utf8.constEnd(), '\n' );
        editor.send( SCI_ALLOCATE, utf8.size() + 1 );
        editor.send( SCI_ALLOCATELINES, newlines + 1 );
    }

    if( knob == Knob::AppendText )
    {
        editor.send( SCI_CLEARALL );
        editor.sends( SCI_APPENDTEXT, utf8.size(), utf8.constData() );
    }
    else
    {
        editor.sends( SCI_SETTEXT, 0, utf8.constData() );
    }

    if( noUndo )
        editor.send( SCI_SETUNDOCOLLECTION, 1 );
    editor.send( SCI_EMPTYUNDOBUFFER );
    editor.send( SCI_SETSAVEPOINT );
    if( changeHistoryFlags != SC_CHANGE_HISTORY_DISABLED )
        editor.send( SCI_SETCHANGEHISTORY, changeHistoryFlags );
}

/// 운영과 같은 초기 설정을 준다. 여기서 재는 값이 백엔드 생성자가 보내는 것과
/// 다른 조건에서 나오면 비교에 쓸 수 없다.
std::unique_ptr< ScintillaEditBase > makeEditor( const bool wrap, const bool visible )
{
    auto editor = std::make_unique< ScintillaEditBase >();
    editor->send( SCI_SETCODEPAGE, SC_CP_UTF8 );
    editor->send( SCI_SETIDLESTYLING, SC_IDLESTYLING_ALL );
    editor->send( SCI_SETLAYOUTCACHE, SC_CACHE_PAGE );
    editor->send( SCI_STYLESETCHECKMONOSPACED, STYLE_DEFAULT, 1 );
    editor->send( SCI_STYLECLEARALL );
    // 운영 기본값은 ChangeHistoryBoth 다 (ScintillaEditorSettings).
    editor->send( SCI_SETCHANGEHISTORY,
                  SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS
                      | SC_CHANGE_HISTORY_INDICATORS );
    editor->send( SCI_SETWRAPMODE, wrap ? SC_WRAP_CHAR : SC_WRAP_NONE );
    if( visible )
    {
        editor->resize( kViewWidth, kViewHeight );
        editor->show();
        QApplication::processEvents();
    }
    return editor;
}

double median( std::vector< double > values )
{
    if( values.empty() )
        return 0.0;
    std::sort( values.begin(), values.end() );
    return values[ values.size() / 2 ];
}

}  // namespace

/// 본문을 넣는 데 드는 시간. wrap 은 끈다 — 켜면 폭 측정 비용이 섞여 적재
/// 자체의 차이가 묻힌다(그쪽은 tst_ScintillaTextMetrics 가 따로 잰다).
void ScintillaDocumentLoadTest::insertCost()
{
    if( qgetenv( "MRST_PERF" ).trimmed().isEmpty() )
        QSKIP( "성능 계측은 MRST_PERF=1 일 때만 돈다 (벽시계 값이라 CI 에서 단언하지 않는다)" );

    static const int  sizes[] = { 2000, 20000, 200000 };
    static const Knob knobs[] = { Knob::Baseline, Knob::NoUndo, Knob::Allocate,
                                  Knob::NoUndoAllocate, Knob::AppendText };

    qInfo().noquote() << QStringLiteral( "\n본문 적재 비용 (ms, 중앙값 %1회, wrap 끔)" ).arg( kRepeat );
    qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4 %5 %6" )
                             .arg( QStringLiteral( "줄 수" ), 8 )
                             .arg( QStringLiteral( "현재" ), 12 )
                             .arg( QStringLiteral( "+undo끔" ), 12 )
                             .arg( QStringLiteral( "+예약" ), 12 )
                             .arg( QStringLiteral( "+undo끔+예약" ), 14 )
                             .arg( QStringLiteral( "+append" ), 12 );

    for( const int lines : sizes )
    {
        const QByteArray corpus = buildCorpus( lines );
        QStringList      cells;
        for( const Knob knob : knobs )
        {
            std::vector< double > samples;
            samples.reserve( kRepeat );
            for( int round = 0; round < kRepeat; ++round )
            {
                // 회차마다 새 편집기 = 빈 문서. 같은 문서를 재사용하면 SplitVector
                // 용량이 남아 예약의 효과가 사라진다.
                auto          editor = makeEditor( false, false );
                QElapsedTimer timer;
                timer.start();
                pushText( *editor, corpus, knob );
                samples.push_back( timer.nsecsElapsed() / 1e6 );
            }
            cells << QString::number( median( std::move( samples ) ), 'f', 2 );
        }

        qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4 %5 %6" )
                                 .arg( QString::number( lines ), 8 )
                                 .arg( cells.at( 0 ), 12 )
                                 .arg( cells.at( 1 ), 12 )
                                 .arg( cells.at( 2 ), 12 )
                                 .arg( cells.at( 3 ), 14 )
                                 .arg( cells.at( 4 ), 12 );
    }

    QVERIFY( true );
}

/// 접기 깊이 주입(`SCI_SETFOLDLEVEL` × 줄 수)이 컨테이너 알림으로 되돌아오는
/// 비용. reST/Markdown 은 렉서가 접기 깊이를 채우지 않으므로 우리가 문서를 훑어
/// 직접 넣는데, 그 하나하나가 `SC_MOD_CHANGEFOLD` 알림이 되어
/// `ScintillaEditBase::modified` 시그널로 올라온다 (그 안에서 `SCI_GETTEXTLENGTH`
/// 호출과 `QByteArray::fromRawData` 할당이 일어난다).
///
/// 우리 핸들러는 텍스트 변경이 아니면 즉시 돌아서지만, 시그널 발화 자체는
/// 이미 치른 값이다. `SCI_SETMODEVENTMASK` 로 미리 거르면 그마저 사라진다.
void ScintillaDocumentLoadTest::foldLevelInjectionCost()
{
    if( qgetenv( "MRST_PERF" ).trimmed().isEmpty() )
        QSKIP( "성능 계측은 MRST_PERF=1 일 때만 돈다" );

    static const int sizes[] = { 2000, 20000, 200000 };

    qInfo().noquote() << QStringLiteral( "\n접기 깊이 주입 비용 (ms, 중앙값 %1회)" ).arg( kRepeat );
    qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4" )
                             .arg( QStringLiteral( "줄 수" ), 8 )
                             .arg( QStringLiteral( "마스크 전체" ), 14 )
                             .arg( QStringLiteral( "마스크 좁힘" ), 14 )
                             .arg( QStringLiteral( "알림 횟수" ), 12 );

    for( const int lines : sizes )
    {
        const QByteArray      corpus = buildCorpus( lines );
        std::vector< double > wide;
        std::vector< double > narrow;
        int                   notifications = 0;

        for( int round = 0; round < kRepeat; ++round )
        {
            for( int narrowMask = 0; narrowMask < 2; ++narrowMask )
            {
                auto editor = makeEditor( false, false );
                pushText( *editor, corpus, Knob::NoUndoAllocate );

                int seen = 0;
                QObject::connect( editor.get(), &ScintillaEditBase::modified, editor.get(),
                                  [ &seen ]( Scintilla::ModificationFlags, Scintilla::Position,
                                             Scintilla::Position, Scintilla::Position,
                                             const QByteArray&, Scintilla::Position,
                                             Scintilla::FoldLevel, Scintilla::FoldLevel ) {
                                      ++seen;
                                  } );
                if( narrowMask )
                    editor->send( SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT );

                const int     lineCount = static_cast< int >( editor->send( SCI_GETLINECOUNT ) );
                QElapsedTimer timer;
                timer.start();
                for( int line = 0; line < lineCount; ++line )
                    editor->send( SCI_SETFOLDLEVEL, line, SC_FOLDLEVELBASE + ( line % 3 ) );
                const double ms = timer.nsecsElapsed() / 1e6;

                if( narrowMask )
                    narrow.push_back( ms );
                else
                {
                    wide.push_back( ms );
                    notifications = seen;
                }
            }
        }

        qInfo().noquote() << QStringLiteral( "%1 %2 %3 %4" )
                                 .arg( QString::number( lines ), 8 )
                                 .arg( QString::number( median( std::move( wide ) ), 'f', 2 ), 14 )
                                 .arg( QString::number( median( std::move( narrow ) ), 'f', 2 ), 14 )
                                 .arg( QString::number( notifications ), 12 );
    }

    QVERIFY( true );
}

/// 적재 경로를 바꿔도 문서가 같은지 지킨다. 이 테스트가 유일한 안전장치다 —
/// 본문이 한 글자 잘리거나 줄 수가 어긋나면 저장 시 사용자 파일이 상한다.
///
/// 특히 노리는 것:
///   - `SCI_SETUNDOCOLLECTION` 을 끄고 넣은 뒤 되돌렸을 때 편집·되돌리기가 되는가
///   - 저장점(`SCI_GETMODIFY`) 이 깨끗한가
///   - 본문 가운데 NUL 이 있을 때 `SCI_SETTEXT` 와 `SCI_APPENDTEXT` 가 갈리는가
void ScintillaDocumentLoadTest::loadedDocumentsAgree()
{
    const QByteArray corpus = buildCorpus( 500 );

    auto reference = makeEditor( false, false );
    pushText( *reference, corpus, Knob::Baseline );
    const int refLength = static_cast< int >( reference->send( SCI_GETLENGTH ) );
    const int refLines = static_cast< int >( reference->send( SCI_GETLINECOUNT ) );
    QCOMPARE( refLength, static_cast< int >( corpus.size() ) );

    for( const Knob knob : { Knob::NoUndo, Knob::Allocate, Knob::NoUndoAllocate, Knob::AppendText } )
    {
        auto editor = makeEditor( false, false );
        pushText( *editor, corpus, knob );

        QCOMPARE( static_cast< int >( editor->send( SCI_GETLENGTH ) ), refLength );
        QCOMPARE( static_cast< int >( editor->send( SCI_GETLINECOUNT ) ), refLines );
        QVERIFY2( editor->send( SCI_GETMODIFY ) == 0,
                  qPrintable( QStringLiteral( "%1: 적재 직후에 문서가 수정 상태다" )
                                  .arg( QString::fromUtf8( knobName( knob ) ) ) ) );
        QVERIFY2( editor->send( SCI_CANUNDO ) == 0,
                  qPrintable( QStringLiteral( "%1: 적재 자체가 되돌리기 이력에 남았다" )
                                  .arg( QString::fromUtf8( knobName( knob ) ) ) ) );

        QByteArray got( refLength + 1, '\0' );
        editor->sends( SCI_GETTEXT, got.size(), got.data() );
        got.resize( refLength );
        QVERIFY2( got == corpus,
                  qPrintable( QStringLiteral( "%1: 본문이 기준과 다르다" )
                                  .arg( QString::fromUtf8( knobName( knob ) ) ) ) );

        // undo 수집을 되돌린 뒤에도 편집이 되돌려져야 한다.
        editor->send( SCI_GOTOPOS, 0 );
        editor->sends( SCI_INSERTTEXT, 0, "x" );
        QVERIFY2( editor->send( SCI_CANUNDO ) != 0,
                  qPrintable( QStringLiteral( "%1: 적재 후 편집이 되돌리기 이력에 안 남는다" )
                                  .arg( QString::fromUtf8( knobName( knob ) ) ) ) );
        editor->send( SCI_UNDO );
        QCOMPARE( static_cast< int >( editor->send( SCI_GETLENGTH ) ), refLength );
    }

    // 본문 가운데 NUL. `SCI_SETTEXT` 은 strlen 으로 길이를 재므로 여기서 잘린다.
    const QByteArray withNul =
        QByteArray( "앞부분\n" ) + QByteArray( 1, '\0' ) + QByteArray( "\n뒷부분\n" );
    {
        auto editor = makeEditor( false, false );
        pushText( *editor, withNul, Knob::Baseline );
        const int length = static_cast< int >( editor->send( SCI_GETLENGTH ) );
        qInfo().noquote() << QStringLiteral( "NUL 포함 %1 바이트: SCI_SETTEXT 는 %2 바이트만 넣는다" )
                                 .arg( withNul.size() )
                                 .arg( length );
    }
    {
        auto editor = makeEditor( false, false );
        pushText( *editor, withNul, Knob::AppendText );
        QCOMPARE( static_cast< int >( editor->send( SCI_GETLENGTH ) ),
                  static_cast< int >( withNul.size() ) );
    }
}

MRST_REGISTER_TEST( ScintillaDocumentLoadTest );

#include "tst_ScintillaDocumentLoad.moc"
