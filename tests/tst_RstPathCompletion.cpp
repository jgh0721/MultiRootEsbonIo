#include "TestRunner.hpp"

#include "core/solRstPathCompletion.hpp"

#include <QTest>

using namespace mrst::rstpath;

/// 경로 완성에서 실제로 문서를 깨뜨리는 것은 후보 목록이 아니라 **삽입 문자열**이다.
/// docutils 는 자리마다 인자를 다르게 변환하므로(uri vs path), 같은 파일 이름이라도
/// image 와 include 에 넣는 문자열이 달라야 한다. 그 규칙을 여기서 고정한다.
class TestRstPathCompletion : public QObject
{
    Q_OBJECT

private slots:
    // ── 슬롯 표 ──
    void argumentSlotsCoverPathDirectives();
    void argumentSlotsRejectFormatAndTitle();
    void optionSlotsCoverFileAndTarget();
    void bodySlotIsToctreeOnly();
    void roleTargetSlots();

    // ── 입력 경로 가르기 ──
    void splitsTrailingSegment_data();
    void splitsTrailingSegment();
    void splitDecodesEscapedSpace();
    void splitMarksSourceRootPaths();

    // ── 삽입 인코딩 ──
    void escapesSpacesForUriSlots();
    void preservesSpacesForPathSlots();
    void alwaysUsesForwardSlash();

    // ── 후보 걸러내기 ──
    void imageSlotAcceptsOnlyImages();
    void openSlotAcceptsAnything();
    void forbiddenSpaceSlotDropsNamesWithSpaces();

    // ── docname ──
    void stripsOnlyRegisteredSourceSuffix();
};

// ── 슬롯 표 ───────────────────────────────────────────────

void TestRstPathCompletion::argumentSlotsCoverPathDirectives()
{
    QVERIFY( slotForArgument( QStringLiteral( "image" ) ) != nullptr );
    QVERIFY( slotForArgument( QStringLiteral( "figure" ) ) != nullptr );
    QVERIFY( slotForArgument( QStringLiteral( "include" ) ) != nullptr );
    QVERIFY( slotForArgument( QStringLiteral( "literalinclude" ) ) != nullptr );
    QVERIFY( slotForArgument( QStringLiteral( "graphviz" ) ) != nullptr );

    // image 인자는 docutils 의 uri() 를 타므로 공백을 이스케이프해야 한다.
    QCOMPARE( int( slotForArgument( QStringLiteral( "image" ) )->spaces ), int( Spaces::Escape ) );
    // include 는 path() 를 타므로 공백을 그대로 둔다.
    QCOMPARE( int( slotForArgument( QStringLiteral( "include" ) )->spaces ),
             int( Spaces::Preserve ) );
}

void TestRstPathCompletion::argumentSlotsRejectFormatAndTitle()
{
    // raw 의 인자는 포맷 이름(html/latex), csv-table 의 인자는 표 제목이다.
    QVERIFY( slotForArgument( QStringLiteral( "raw" ) ) == nullptr );
    QVERIFY( slotForArgument( QStringLiteral( "csv-table" ) ) == nullptr );
    QVERIFY( slotForArgument( QStringLiteral( "note" ) ) == nullptr );
    QVERIFY( slotForArgument( QStringLiteral( "code-block" ) ) == nullptr );
}

void TestRstPathCompletion::optionSlotsCoverFileAndTarget()
{
    QVERIFY( slotForOption( QStringLiteral( "csv-table" ), QStringLiteral( "file" ) ) != nullptr );
    QVERIFY( slotForOption( QStringLiteral( "raw" ), QStringLiteral( "file" ) ) != nullptr );
    QVERIFY( slotForOption( QStringLiteral( "image" ), QStringLiteral( "target" ) ) != nullptr );
    QVERIFY( slotForOption( QStringLiteral( "literalinclude" ), QStringLiteral( "diff" ) ) != nullptr );

    // 소유 directive 가 다르면 같은 옵션 이름이라도 경로가 아니다.
    QVERIFY( slotForOption( QStringLiteral( "note" ), QStringLiteral( "file" ) ) == nullptr );
    // :url: 은 원격 주소다. 로컬 파일을 제안하면 틀린 후보가 된다.
    QVERIFY( slotForOption( QStringLiteral( "csv-table" ), QStringLiteral( "url" ) ) == nullptr );
}

void TestRstPathCompletion::bodySlotIsToctreeOnly()
{
    const Slot* toctree = slotForBody( QStringLiteral( "toctree" ) );
    QVERIFY( toctree != nullptr );
    QCOMPARE( int( toctree->shape ), int( Shape::DocName ) );
    QVERIFY( slotForBody( QStringLiteral( "note" ) ) == nullptr );
    QVERIFY( slotForBody( QStringLiteral( "code-block" ) ) == nullptr );
}

void TestRstPathCompletion::roleTargetSlots()
{
    QVERIFY( slotForRoleTarget( QStringLiteral( "download" ) ) != nullptr );
    QCOMPARE( int( slotForRoleTarget( QStringLiteral( "doc" ) )->shape ), int( Shape::DocName ) );
    // :ref: 대상은 라벨이지 경로가 아니다.
    QVERIFY( slotForRoleTarget( QStringLiteral( "ref" ) ) == nullptr );
    QVERIFY( slotForRoleTarget( QStringLiteral( "term" ) ) == nullptr );
}

// ── 입력 경로 가르기 ──────────────────────────────────────

void TestRstPathCompletion::splitsTrailingSegment_data()
{
    QTest::addColumn< QString >( "typed" );
    QTest::addColumn< QString >( "directory" );
    QTest::addColumn< QString >( "name" );

    QTest::newRow( "구분자 없음" ) << "logo" << "" << "logo";
    QTest::newRow( "한 단계" ) << "img/logo" << "img" << "logo";
    QTest::newRow( "위로" ) << "../img/lo" << "../img" << "lo";
    QTest::newRow( "여러 단계 위로" ) << "../../../a/b" << "../../../a" << "b";
    QTest::newRow( "끝이 구분자" ) << "img/" << "img" << "";
    QTest::newRow( "백슬래시" ) << "..\\img\\lo" << "../img" << "lo";
    QTest::newRow( "소스 루트" ) << "/_static/lo" << "/_static" << "lo";
}

void TestRstPathCompletion::splitsTrailingSegment()
{
    QFETCH( QString, typed );
    QFETCH( QString, directory );
    QFETCH( QString, name );

    const TypedPath split = splitTypedPath( typed );
    QCOMPARE( split.directory, directory );
    QCOMPARE( split.name, name );
}

void TestRstPathCompletion::splitDecodesEscapedSpace()
{
    // reST 에서 백슬래시+공백은 이스케이프된 공백이다. 경로 구분자로 보면
    // "my\ photo.png" 가 두 조각으로 잘려 팝업 필터가 엉뚱한 글자를 받는다.
    const TypedPath split = splitTypedPath( QStringLiteral( "img/my\\ pho" ) );
    QCOMPARE( split.directory, QStringLiteral( "img" ) );
    QCOMPARE( split.name, QStringLiteral( "my pho" ) );
}

void TestRstPathCompletion::splitMarksSourceRootPaths()
{
    QVERIFY( splitTypedPath( QStringLiteral( "/_static/a" ) ).fromSourceRoot );
    QVERIFY( splitTypedPath( QStringLiteral( "\\_static\\a" ) ).fromSourceRoot );
    QVERIFY( !splitTypedPath( QStringLiteral( "_static/a" ) ).fromSourceRoot );
    QVERIFY( !splitTypedPath( QStringLiteral( "../a" ) ).fromSourceRoot );
}

// ── 삽입 인코딩 ───────────────────────────────────────────

void TestRstPathCompletion::escapesSpacesForUriSlots()
{
    const Slot* image = slotForArgument( QStringLiteral( "image" ) );
    QVERIFY( image != nullptr );

    // docutils 의 uri() 는 이스케이프하지 않은 공백을 통째로 지운다. %20 은
    // relfn2path 가 퍼센트 디코딩을 하지 않아 존재하지 않는 파일을 가리킨다.
    QCOMPARE( encodeForInsertion( QStringLiteral( "img/my photo.png" ), *image ),
             QStringLiteral( "img/my\\ photo.png" ) );
}

void TestRstPathCompletion::preservesSpacesForPathSlots()
{
    const Slot* include = slotForArgument( QStringLiteral( "include" ) );
    QVERIFY( include != nullptr );
    QCOMPARE( encodeForInsertion( QStringLiteral( "docs/my note.rst" ), *include ),
             QStringLiteral( "docs/my note.rst" ) );
}

void TestRstPathCompletion::alwaysUsesForwardSlash()
{
    const Slot* include = slotForArgument( QStringLiteral( "include" ) );
    QCOMPARE( encodeForInsertion( QStringLiteral( "..\\img\\a.rst" ), *include ),
             QStringLiteral( "../img/a.rst" ) );
}

// ── 후보 걸러내기 ─────────────────────────────────────────

void TestRstPathCompletion::imageSlotAcceptsOnlyImages()
{
    const Slot* image = slotForArgument( QStringLiteral( "image" ) );
    QVERIFY( acceptsFileName( QStringLiteral( "logo.png" ), *image ) );
    QVERIFY( acceptsFileName( QStringLiteral( "LOGO.WEBP" ), *image ) );
    QVERIFY( !acceptsFileName( QStringLiteral( "index.rst" ), *image ) );
    QVERIFY( !acceptsFileName( QStringLiteral( "notes.txt" ), *image ) );
    // 공백은 이스케이프로 살릴 수 있으므로 후보에서 빼지 않는다.
    QVERIFY( acceptsFileName( QStringLiteral( "my photo.jpg" ), *image ) );
}

void TestRstPathCompletion::openSlotAcceptsAnything()
{
    // literalinclude 와 :download: 는 실제로 무엇이든 받는다. 확장자로 거르면
    // 정당한 파일을 감추게 된다.
    const Slot* literal = slotForArgument( QStringLiteral( "literalinclude" ) );
    QVERIFY( acceptsFileName( QStringLiteral( "main.cpp" ), *literal ) );
    QVERIFY( acceptsFileName( QStringLiteral( "CMakeLists.txt" ), *literal ) );
    QVERIFY( acceptsFileName( QStringLiteral( "무확장자" ), *literal ) );
}

void TestRstPathCompletion::forbiddenSpaceSlotDropsNamesWithSpaces()
{
    const Slot* toctree = slotForBody( QStringLiteral( "toctree" ) );
    QVERIFY( acceptsFileName( QStringLiteral( "intro.rst" ), *toctree ) );
    // toctree 항목에 공백이 들어가면 Sphinx 가 항목을 못 찾는다.
    QVERIFY( !acceptsFileName( QStringLiteral( "getting started.rst" ), *toctree ) );
    QVERIFY( !acceptsFileName( QStringLiteral( "logo.png" ), *toctree ) );
}

// ── docname ───────────────────────────────────────────────

void TestRstPathCompletion::stripsOnlyRegisteredSourceSuffix()
{
    QCOMPARE( stripDocumentSuffix( QStringLiteral( "intro.rst" ) ), QStringLiteral( "intro" ) );
    QCOMPARE( stripDocumentSuffix( QStringLiteral( "guide.md" ) ), QStringLiteral( "guide" ) );
    // Sphinx 는 등록된 접미사만 removesuffix 한다. 점이 여럿이어도 앞은 남는다.
    QCOMPARE( stripDocumentSuffix( QStringLiteral( "api.v2.rst" ) ), QStringLiteral( "api.v2" ) );
    // 문서 확장자가 아니면 손대지 않는다.
    QCOMPARE( stripDocumentSuffix( QStringLiteral( "logo.png" ) ), QStringLiteral( "logo.png" ) );
}

MRST_REGISTER_TEST( TestRstPathCompletion );

#include "tst_RstPathCompletion.moc"
