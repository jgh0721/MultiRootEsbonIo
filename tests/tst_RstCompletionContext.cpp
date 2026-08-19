#include "TestRunner.hpp"

#include "core/solRstOfflineCompletions.hpp"

#include <QSet>
#include <QTest>

using namespace mrst::rstcomplete;

/// 자동완성에서 실제로 틀리기 쉬운 건 후보 목록이 아니라 "지금 무엇을 완성해야
/// 하는가" 판정이다. 잘못 판정하면 엉뚱한 목록이 뜨거나 아예 안 뜬다.
class TestRstCompletionContext : public QObject
{
    Q_OBJECT

private slots:
    void detectsDirective_data();
    void detectsDirective();
    void detectsRole();
    void detectsDirectiveOption();
    void directiveOptionNeedsEnclosingDirective();
    void detectsRoleTarget();
    void detectsPathArgument();
    void pathOnlyForDirectivesThatTakeOne();
    void noContextInPlainText();
    void filtersByPrefix();
    void tablesAreNonEmpty();

    // ── LSP 응답 다듬기 ──
    void stripsLeadingSpaceInDirectiveContext();
    void keepsLeadingSpaceOutsideDirectiveContext();
    void keepsTrailingSpaceOfInsertText();
    void dropsDuplicateAndEmptyInsertText();
    void stripsSnippetControlCharacter();
    void limitsItemCount();
    void mergeKeepsPrimaryOrderAndSkipsDuplicates();

    // ── 경로 슬롯 (2026-08 정정) ──
    void rejectsRawFormatArgument();
    void rejectsCsvTableTitleArgument();
    void detectsPathInOptionValue_data();
    void detectsPathInOptionValue();
    void optionValuePathNeedsMatchingOwner();
    void detectsToctreeBodyEntry();
    void toctreeBodyNeedsToctreeOwner();
    void detectsGraphvizArgument();
    void graphvizGivesUpOnSpace();
    void detectsDownloadRoleTarget();
    void detectsDocRoleTarget();
    void keepsPlainRoleTargetForRef();

    // ── 필터 접두 / 치환 길이 ──
    void filterPrefixIsLastSegment_data();
    void filterPrefixIsLastSegment();
    void filterPrefixEqualsPrefixOutsidePathContext();
    void trailingSeparatorGivesEmptyFilterPrefix();

    // ── 정규식 수정 ──
    void detectsPathWithSpaces();
    void detectsPathRightAfterDoubleColon();
    void detectsSubstitutionImagePath();
};

void TestRstCompletionContext::detectsDirective_data()
{
    QTest::addColumn< QString >( "line" );
    QTest::addColumn< int >( "column" );
    QTest::addColumn< QString >( "prefix" );

    QTest::newRow( "빈 directive" ) << ".. " << 4 << "";
    QTest::newRow( "일부 입력" ) << ".. no" << 6 << "no";
    QTest::newRow( "들여쓴 directive" ) << "   .. cod" << 10 << "cod";
    QTest::newRow( "하이픈 포함" ) << ".. code-bl" << 11 << "code-bl";
}

void TestRstCompletionContext::detectsDirective()
{
    QFETCH( QString, line );
    QFETCH( int, column );
    QFETCH( QString, prefix );

    const Context context = detectContext( line, column );
    QCOMPARE( int( context.kind ), int( ContextKind::Directive ) );
    QCOMPARE( context.prefix, prefix );
    QCOMPARE( context.replaceLength, prefix.length() );
}

void TestRstCompletionContext::detectsRole()
{
    const Context context = detectContext( QStringLiteral( "본문에 :re" ), 11 );
    QCOMPARE( int( context.kind ), int( ContextKind::Role ) );
    QCOMPARE( context.prefix, QStringLiteral( "re" ) );

    const QVector< Item > items = candidatesFor( context );
    QVERIFY( !items.isEmpty() );
    bool hasRef = false;
    for( const Item& item : items )
        hasRef = hasRef || item.label == QStringLiteral( "ref" );
    QVERIFY2( hasRef, "':re' 에 ref 가 후보로 나와야 한다" );
}

void TestRstCompletionContext::detectsDirectiveOption()
{
    const QStringList previous{ QStringLiteral( ".. code-block:: python" ) };
    const Context context = detectContext( QStringLiteral( "   :lineno" ), 11, previous );

    QCOMPARE( int( context.kind ), int( ContextKind::DirectiveOption ) );
    QCOMPARE( context.directiveName, QStringLiteral( "code-block" ) );
    QCOMPARE( context.prefix, QStringLiteral( "lineno" ) );

    const QVector< Item > items = candidatesFor( context );
    QVERIFY2( !items.isEmpty(), "code-block 의 linenos 옵션이 나와야 한다" );
}

void TestRstCompletionContext::directiveOptionNeedsEnclosingDirective()
{
    // 앞에 directive 가 없으면 그냥 필드 목록이지 옵션이 아니다.
    const QStringList previous{ QStringLiteral( "그냥 문단입니다." ) };
    const Context context = detectContext( QStringLiteral( "   :author" ), 11, previous );
    QVERIFY( context.kind != ContextKind::DirectiveOption );
}

void TestRstCompletionContext::detectsRoleTarget()
{
    const Context context = detectContext( QStringLiteral( "보기 :ref:`my-la" ), 16 );
    QCOMPARE( int( context.kind ), int( ContextKind::RoleTarget ) );
    QCOMPARE( context.directiveName, QStringLiteral( "ref" ) );
    QCOMPARE( context.prefix, QStringLiteral( "my-la" ) );
}

void TestRstCompletionContext::detectsPathArgument()
{
    const Context context = detectContext( QStringLiteral( ".. image:: _ima" ), 16 );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( context.directiveName, QStringLiteral( "image" ) );
    QCOMPARE( context.prefix, QStringLiteral( "_ima" ) );
}

void TestRstCompletionContext::pathOnlyForDirectivesThatTakeOne()
{
    // note 는 경로를 받지 않는다.
    const Context context = detectContext( QStringLiteral( ".. note:: 어떤" ), 14 );
    QVERIFY( context.kind != ContextKind::Path );
}

void TestRstCompletionContext::noContextInPlainText()
{
    QCOMPARE( int( detectContext( QStringLiteral( "그냥 평범한 문장입니다." ), 12 ).kind ),
             int( ContextKind::None ) );
    QCOMPARE( int( detectContext( QString{}, 1 ).kind ), int( ContextKind::None ) );
}

void TestRstCompletionContext::filtersByPrefix()
{
    const Context all = detectContext( QStringLiteral( ".. " ), 4 );
    const Context narrowed = detectContext( QStringLiteral( ".. code" ), 8 );

    const int allCount = candidatesFor( all ).size();
    const QVector< Item > filtered = candidatesFor( narrowed );

    QVERIFY( allCount > filtered.size() );
    QVERIFY( !filtered.isEmpty() );
    for( const Item& item : filtered )
        QVERIFY2( item.label.startsWith( QStringLiteral( "code" ) ), qPrintable( item.label ) );
}

void TestRstCompletionContext::tablesAreNonEmpty()
{
    const QStringList directives = knownDirectives();
    const QStringList roles = knownRoles();

    QVERIFY( directives.size() > 20 );
    QVERIFY( roles.size() > 20 );

    // 실제로 자주 쓰는 것이 빠지면 오프라인 폴백의 의미가 없다.
    for( const char* expected : { "note", "warning", "code-block", "toctree", "image", "list-table" } )
        QVERIFY2( directives.contains( QLatin1String( expected ) ), expected );
    for( const char* expected : { "ref", "doc", "kbd", "math" } )
        QVERIFY2( roles.contains( QLatin1String( expected ) ), expected );

    QCOMPARE( directives.size(), QSet< QString >( directives.begin(), directives.end() ).size() );
    QCOMPARE( roles.size(), QSet< QString >( roles.begin(), roles.end() ).size() );
}

// ── LSP 응답 다듬기 ────────────────────────────────────────

namespace {

QVector< Item > items( const QStringList& insertTexts )
{
    QVector< Item > result;
    for( const QString& text : insertTexts )
        result.push_back( { text.trimmed(), text, QString{}, 0 } );
    return result;
}

QStringList insertTexts( const QVector< Item >& source )
{
    QStringList result;
    for( const Item& item : source )
        result << item.insertText;
    return result;
}

}  // namespace

void TestRstCompletionContext::stripsLeadingSpaceInDirectiveContext()
{
    // Esbonio 는 ".." 직후 발화를 전제로 " image::" 를 돌려준다.
    // ".. " 까지 친 상태에서 그대로 넣으면 "..  image::" 가 된다.
    const QVector< Item > normalized =
        normalizeLspItems( items( { QStringLiteral( " image::" ), QStringLiteral( " note::" ) } ),
                          QStringLiteral( ".. " ), 4 );

    QCOMPARE( insertTexts( normalized ),
             QStringList( { QStringLiteral( "image::" ), QStringLiteral( "note::" ) } ) );
}

void TestRstCompletionContext::keepsLeadingSpaceOutsideDirectiveContext()
{
    const QVector< Item > normalized =
        normalizeLspItems( items( { QStringLiteral( " ref:" ) } ), QStringLiteral( "본문 :" ), 7 );
    QCOMPARE( insertTexts( normalized ), QStringList( { QStringLiteral( " ref:" ) } ) );
}

void TestRstCompletionContext::keepsTrailingSpaceOfInsertText()
{
    // 끝 공백은 캐럿을 인자 자리로 보내는 의미가 있다. 잘라내면 안 된다.
    const QVector< Item > normalized =
        normalizeLspItems( items( { QStringLiteral( " code-block:: " ) } ),
                          QStringLiteral( ".. co" ), 6 );
    QCOMPARE( insertTexts( normalized ), QStringList( { QStringLiteral( "code-block:: " ) } ) );
}

void TestRstCompletionContext::dropsDuplicateAndEmptyInsertText()
{
    const QVector< Item > cleaned = finalizeItems( items( { QStringLiteral( "note::" ),
                                                           QString{},
                                                           QStringLiteral( "note::" ),
                                                           QStringLiteral( "tip::" ) } ) );
    QCOMPARE( insertTexts( cleaned ),
             QStringList( { QStringLiteral( "note::" ), QStringLiteral( "tip::" ) } ) );
}

void TestRstCompletionContext::stripsSnippetControlCharacter()
{
    const QString withControl = QStringLiteral( "image::" ) + QChar( 0x0001 );
    const QVector< Item > cleaned = finalizeItems( items( { withControl } ) );
    QCOMPARE( insertTexts( cleaned ), QStringList( { QStringLiteral( "image::" ) } ) );
}

void TestRstCompletionContext::limitsItemCount()
{
    QStringList many;
    for( int index = 0; index < 500; ++index )
        many << QStringLiteral( "item%1" ).arg( index );

    QCOMPARE( finalizeItems( items( many ) ).size(), 200 );
    QCOMPARE( finalizeItems( items( many ), 5 ).size(), 5 );
}

void TestRstCompletionContext::mergeKeepsPrimaryOrderAndSkipsDuplicates()
{
    const QVector< Item > merged =
        mergeItems( items( { QStringLiteral( "b" ), QStringLiteral( "a" ) } ),
                   items( { QStringLiteral( "a" ), QStringLiteral( "c" ) } ) );

    QCOMPARE( insertTexts( merged ), QStringList( { QStringLiteral( "b" ), QStringLiteral( "a" ),
                                                   QStringLiteral( "c" ) } ) );
}

// ── 경로 슬롯 ─────────────────────────────────────────────
//
// 예전 takesPathArgument() 는 raw 와 csv-table 의 **인자**를 경로로 봤다.
// 둘 다 아니다. raw 의 인자는 포맷 이름이고 csv-table 의 인자는 표 제목이다.

void TestRstCompletionContext::rejectsRawFormatArgument()
{
    const Context context = detectContext( QStringLiteral( ".. raw:: ht" ), 12 );
    QVERIFY( context.kind != ContextKind::Path );
}

void TestRstCompletionContext::rejectsCsvTableTitleArgument()
{
    const Context context = detectContext( QStringLiteral( ".. csv-table:: 매출 표" ), 21 );
    QVERIFY( context.kind != ContextKind::Path );
}

void TestRstCompletionContext::detectsPathInOptionValue_data()
{
    QTest::addColumn< QString >( "owner" );
    QTest::addColumn< QString >( "line" );
    QTest::addColumn< int >( "column" );
    QTest::addColumn< QString >( "option" );
    QTest::addColumn< QString >( "prefix" );

    QTest::newRow( "csv-table :file:" )
        << ".. csv-table:: 매출" << "   :file: dat" << 15 << "file" << "dat";
    QTest::newRow( "raw :file:" ) << ".. raw:: html" << "   :file: sni" << 15 << "file" << "sni";
    QTest::newRow( "image :target:" )
        << ".. image:: a.png" << "   :target: fu" << 16 << "target" << "fu";
    QTest::newRow( "literalinclude :diff:" )
        << ".. literalinclude:: a.py" << "   :diff: ol" << 14 << "diff" << "ol";
}

void TestRstCompletionContext::detectsPathInOptionValue()
{
    QFETCH( QString, owner );
    QFETCH( QString, line );
    QFETCH( int, column );
    QFETCH( QString, option );
    QFETCH( QString, prefix );

    const Context context = detectContext( line, column, { QString{}, owner } );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( int( context.pathSite ), int( PathSlotSite::Option ) );
    QCOMPARE( context.optionName, option );
    QCOMPARE( context.prefix, prefix );
}

void TestRstCompletionContext::optionValuePathNeedsMatchingOwner()
{
    // note 에는 :file: 옵션이 없다. 소유 directive 를 확인하지 않으면 평범한
    // 들여쓴 필드 목록에서도 파일 목록이 튀어나온다.
    const Context context =
        detectContext( QStringLiteral( "   :file: dat" ), 14, { QString{}, QStringLiteral( ".. note::" ) } );
    QVERIFY( context.kind != ContextKind::Path );
}

void TestRstCompletionContext::detectsToctreeBodyEntry()
{
    const Context context = detectContext( QStringLiteral( "   guide/set" ), 13,
                                          { QStringLiteral( "   :maxdepth: 2" ),
                                           QStringLiteral( ".. toctree::" ) } );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( int( context.pathSite ), int( PathSlotSite::Body ) );
    QCOMPARE( context.directiveName, QStringLiteral( "toctree" ) );
    QCOMPARE( context.prefix, QStringLiteral( "guide/set" ) );
    QCOMPARE( context.filterPrefix, QStringLiteral( "set" ) );
}

void TestRstCompletionContext::toctreeBodyNeedsToctreeOwner()
{
    // 평범한 들여쓴 본문 한 단어. 여기서 팝업이 뜨면 산문을 쓸 수가 없다.
    const Context context =
        detectContext( QStringLiteral( "   메모" ), 6, { QString{}, QStringLiteral( ".. note::" ) } );
    QVERIFY( context.kind != ContextKind::Path );
}

void TestRstCompletionContext::detectsGraphvizArgument()
{
    const Context context = detectContext( QStringLiteral( ".. graphviz:: dia" ), 18 );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( context.directiveName, QStringLiteral( "graphviz" ) );
}

void TestRstCompletionContext::graphvizGivesUpOnSpace()
{
    // graphviz 는 final_argument_whitespace 가 꺼져 있다. 공백이 보이면
    // 경로를 쓰는 중이 아니다.
    const Context context = detectContext( QStringLiteral( ".. graphviz:: dia gram" ), 23 );
    QVERIFY( context.kind != ContextKind::Path );
}

void TestRstCompletionContext::detectsDownloadRoleTarget()
{
    const Context context = detectContext( QStringLiteral( "받기 :download:`arch/dia" ), 24 );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( int( context.pathSite ), int( PathSlotSite::RoleTarget ) );
    QCOMPARE( context.prefix, QStringLiteral( "arch/dia" ) );
    QCOMPARE( context.filterPrefix, QStringLiteral( "dia" ) );
}

void TestRstCompletionContext::detectsDocRoleTarget()
{
    const Context context = detectContext( QStringLiteral( ":doc:`guide/int" ), 16 );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( int( context.pathSite ), int( PathSlotSite::RoleTarget ) );
}

void TestRstCompletionContext::keepsPlainRoleTargetForRef()
{
    // :ref: 대상은 경로가 아니라 라벨이다. 여기까지 Path 로 바뀌면 안 된다.
    const Context context = detectContext( QStringLiteral( ":ref:`my-la" ), 12 );
    QCOMPARE( int( context.kind ), int( ContextKind::RoleTarget ) );
}

// ── 필터 접두 / 치환 길이 ─────────────────────────────────

void TestRstCompletionContext::filterPrefixIsLastSegment_data()
{
    QTest::addColumn< QString >( "typed" );
    QTest::addColumn< QString >( "filterPrefix" );

    QTest::newRow( "상대 경로" ) << "../img/lo" << "lo";
    QTest::newRow( "백슬래시" ) << "..\\img\\lo" << "lo";
    QTest::newRow( "구분자 없음" ) << "logo" << "logo";
    QTest::newRow( "소스 루트 절대" ) << "/_static/lo" << "lo";
    QTest::newRow( "이스케이프한 공백" ) << "img/my\\ pho" << "my pho";
}

void TestRstCompletionContext::filterPrefixIsLastSegment()
{
    QFETCH( QString, typed );
    QFETCH( QString, filterPrefix );

    const QString line = QStringLiteral( ".. image:: " ) + typed;
    const Context context = detectContext( line, int( line.length() ) + 1 );

    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( context.prefix, typed );
    // 지울 길이는 언제나 친 것 전체다. 항목의 insertText 가 문서 기준 전체
    // 상대 경로이기 때문이다.
    QCOMPARE( context.replaceLength, int( typed.length() ) );
    QCOMPARE( context.filterPrefix, filterPrefix );
}

void TestRstCompletionContext::filterPrefixEqualsPrefixOutsidePathContext()
{
    const Context directive = detectContext( QStringLiteral( ".. code-bl" ), 11 );
    QCOMPARE( directive.filterPrefix, directive.prefix );

    const Context target = detectContext( QStringLiteral( ":ref:`my-la" ), 12 );
    QCOMPARE( target.filterPrefix, target.prefix );
}

void TestRstCompletionContext::trailingSeparatorGivesEmptyFilterPrefix()
{
    const Context context = detectContext( QStringLiteral( ".. image:: img/" ), 16 );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( context.filterPrefix, QString{} );
    QCOMPARE( context.replaceLength, 4 );
}

// ── 정규식 수정 ───────────────────────────────────────────

void TestRstCompletionContext::detectsPathWithSpaces()
{
    // image 는 final_argument_whitespace 가 켜져 있어 공백이 정당하다.
    // 예전 정규식은 (\S*) 라 공백을 친 순간 컨텍스트를 잃었다.
    const Context context = detectContext( QStringLiteral( ".. image:: my pho" ), 18 );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( context.prefix, QStringLiteral( "my pho" ) );
    QCOMPARE( context.filterPrefix, QStringLiteral( "my pho" ) );
}

void TestRstCompletionContext::detectsPathRightAfterDoubleColon()
{
    const Context context = detectContext( QStringLiteral( ".. image::" ), 11 );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( context.prefix, QString{} );
    QVERIFY( context.argumentNeedsSpace );

    const Context spaced = detectContext( QStringLiteral( ".. image:: " ), 12 );
    QCOMPARE( int( spaced.kind ), int( ContextKind::Path ) );
    QVERIFY( !spaced.argumentNeedsSpace );
}

void TestRstCompletionContext::detectsSubstitutionImagePath()
{
    const Context context = detectContext( QStringLiteral( ".. |logo| image:: img/l" ), 24 );
    QCOMPARE( int( context.kind ), int( ContextKind::Path ) );
    QCOMPARE( context.directiveName, QStringLiteral( "image" ) );
    QCOMPARE( context.filterPrefix, QStringLiteral( "l" ) );
}

MRST_REGISTER_TEST( TestRstCompletionContext );

#include "tst_RstCompletionContext.moc"
