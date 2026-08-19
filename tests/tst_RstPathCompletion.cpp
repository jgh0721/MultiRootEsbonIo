#include "TestRunner.hpp"

#include "core/solRstOfflineCompletions.hpp"
#include "core/solRstPathCompletion.hpp"
#include "core/solRstPathIndex.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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

    // ── 디렉터리 해석 ──
    void resolvesRelativeToDocumentDirectory();
    void resolvesLeadingSlashAgainstSourceRoot();
    void resolvesBackslashLeadingPathAgainstSourceRoot();

    // ── 한 단계 후보 (디스크 없이) ──
    void listsDirectoriesBeforeFiles();
    void filtersFilesByAcceptedExtensions();
    void labelIsNameAndInsertTextIsFullRelativePath();
    void keepsSourceRootFormWhenUserTypedLeadingSlash();
    void marksImageFilesWithOwnKind();
    void ranksCurrentDirectoryAboveNothingAndPrefersMatchingExtensions();
    void offersParentDirectory();
    void docNameSlotStripsSuffixAndStaysInsideSourceRoot();
    void escapesSpacesInInsertTextForImageSlot();
    void prependsSpaceWhenArgumentHasNone();

    // ── 전역 퍼지 후보 ──
    void findsFileAnywhereByNameAlone();
    void skipsGlobalSearchWhenNameTooShort();
    void globalCandidatesRespectSlotExtensions();
    void globalCandidatesCarryRelativeDirectoryInDetail();
    void globalCandidatesRankShallowerPathsFirst();
    void docNameSlotRejectsPathsOutsideSourceRoot();
    void mergeKeepsOneLevelFirstAndDropsDuplicates();

    // ── 인덱스 순회 ──
    void scanSkipsExcludedDirectoriesAtAnyDepth();
    void scanReturnsRootRelativeForwardSlashPaths();
    void scanRespectsLimit();
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

// ── 후보 수집 ─────────────────────────────────────────────
//
// DirectoryLister 를 주입해 디스크 없이 돌린다. 실제로 버그가 나는 곳은
// 경로 문자열 계산·확장자 필터·순위·인코딩이고, 그건 전부 이 seam 하나로 덮인다.

namespace {

constexpr const char* kDocumentDirectory = "C:/proj/docs/guide";
constexpr const char* kSourceRoot = "C:/proj/docs";

Query makeQuery( const QString& line, const int column )
{
    Query query;
    query.context = mrst::rstcomplete::detectContext( line, column );
    query.documentDirectory = QString::fromLatin1( kDocumentDirectory );
    query.sourceRoot = QString::fromLatin1( kSourceRoot );
    query.workspaceRoot = QStringLiteral( "C:/proj" );
    return query;
}

/// 항상 같은 것을 돌려주는 가짜 디렉터리. 어느 경로를 물었는지도 기록한다.
DirectoryLister fakeLister( const QVector< DirEntry >& entries, QString* askedFor = nullptr )
{
    return [ entries, askedFor ]( const QString& directory ) {
        if( askedFor != nullptr )
            *askedFor = directory;
        return entries;
    };
}

QStringList labelsOf( const QVector< Candidate >& candidates )
{
    QStringList labels;
    for( const Candidate& candidate : candidates )
        labels << candidate.label;
    return labels;
}

const Candidate* findByLabel( const QVector< Candidate >& candidates, const QString& label )
{
    for( const Candidate& candidate : candidates )
    {
        if( candidate.label == label )
            return &candidate;
    }
    return nullptr;
}

const QVector< DirEntry >& sampleEntries()
{
    static const QVector< DirEntry > entries{
        { QStringLiteral( "logo.png" ), false }, { QStringLiteral( "intro.rst" ), false },
        { QStringLiteral( "notes.txt" ), false }, { QStringLiteral( "icons" ), true },
        { QStringLiteral( "assets" ), true },
    };
    return entries;
}

}   // namespace

void TestRstPathCompletion::resolvesRelativeToDocumentDirectory()
{
    QString asked;
    const Query query = makeQuery( QStringLiteral( ".. image:: ../img/lo" ), 21 );
    oneLevelCandidates( query, fakeLister( {}, &asked ) );
    QCOMPARE( asked, QStringLiteral( "C:/proj/docs/img" ) );
}

void TestRstPathCompletion::resolvesLeadingSlashAgainstSourceRoot()
{
    // Sphinx 에서 "/" 로 시작하는 경로는 srcdir 기준 절대 경로다.
    // C++ 쪽에는 이 변환이 아예 없었다.
    QString asked;
    const Query query = makeQuery( QStringLiteral( ".. image:: /_static/lo" ), 23 );
    oneLevelCandidates( query, fakeLister( {}, &asked ) );
    QCOMPARE( asked, QStringLiteral( "C:/proj/docs/_static" ) );
}

void TestRstPathCompletion::resolvesBackslashLeadingPathAgainstSourceRoot()
{
    // relfn2path 는 백슬래시로 시작해도 srcdir 기준으로 친다.
    QString asked;
    const Query query = makeQuery( QStringLiteral( ".. image:: \\_static\\lo" ), 23 );
    oneLevelCandidates( query, fakeLister( {}, &asked ) );
    QCOMPARE( asked, QStringLiteral( "C:/proj/docs/_static" ) );
}

void TestRstPathCompletion::listsDirectoriesBeforeFiles()
{
    const Query query = makeQuery( QStringLiteral( ".. include:: " ), 14 );
    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( sampleEntries() ) );

    // "..", 디렉터리(사전순), 그다음 파일.
    QCOMPARE( labelsOf( candidates ),
             QStringList( { QStringLiteral( ".." ), QStringLiteral( "assets" ),
                           QStringLiteral( "icons" ), QStringLiteral( "intro.rst" ),
                           QStringLiteral( "logo.png" ), QStringLiteral( "notes.txt" ) } ) );
}

void TestRstPathCompletion::filtersFilesByAcceptedExtensions()
{
    const Query query = makeQuery( QStringLiteral( ".. image:: " ), 12 );
    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( sampleEntries() ) );

    QVERIFY( findByLabel( candidates, QStringLiteral( "logo.png" ) ) != nullptr );
    // 이미지 자리에 .rst 를 제안하면 안 된다. Esbonio 는 이걸 거르지 않는다.
    QVERIFY( findByLabel( candidates, QStringLiteral( "intro.rst" ) ) == nullptr );
    QVERIFY( findByLabel( candidates, QStringLiteral( "notes.txt" ) ) == nullptr );
    // 디렉터리는 확장자와 무관하게 남는다. 더 파고들 곳이기 때문이다.
    QVERIFY( findByLabel( candidates, QStringLiteral( "icons" ) ) != nullptr );
}

void TestRstPathCompletion::labelIsNameAndInsertTextIsFullRelativePath()
{
    const Query query = makeQuery( QStringLiteral( ".. image:: ../img/lo" ), 21 );
    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( sampleEntries() ) );

    const Candidate* logo = findByLabel( candidates, QStringLiteral( "logo.png" ) );
    QVERIFY( logo != nullptr );
    // 라벨은 이름뿐 - 팝업의 퍼지 필터가 마지막 조각만 받기 때문이다.
    QCOMPARE( logo->label, QStringLiteral( "logo.png" ) );
    // insertText 는 친 것 전체를 대신할 문서 기준 상대 경로다.
    QCOMPARE( logo->insertText, QStringLiteral( "../img/logo.png" ) );

    const Candidate* icons = findByLabel( candidates, QStringLiteral( "icons" ) );
    QVERIFY( icons != nullptr );
    // 디렉터리는 끝에 '/' 를 붙여 연속 완성이 자연스럽게 이어지도록 한다.
    QCOMPARE( icons->insertText, QStringLiteral( "../img/icons/" ) );
}

void TestRstPathCompletion::keepsSourceRootFormWhenUserTypedLeadingSlash()
{
    const Query query = makeQuery( QStringLiteral( ".. image:: /_static/lo" ), 23 );
    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( sampleEntries() ) );

    const Candidate* logo = findByLabel( candidates, QStringLiteral( "logo.png" ) );
    QVERIFY( logo != nullptr );
    // 사용자가 고른 표기를 뒤엎지 않는다.
    QCOMPARE( logo->insertText, QStringLiteral( "/_static/logo.png" ) );
}

void TestRstPathCompletion::marksImageFilesWithOwnKind()
{
    const Query query = makeQuery( QStringLiteral( ".. include:: " ), 14 );
    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( sampleEntries() ) );

    QCOMPARE( findByLabel( candidates, QStringLiteral( "logo.png" ) )->kind,
             mrst::rstcomplete::kKindImageFile );
    QCOMPARE( findByLabel( candidates, QStringLiteral( "intro.rst" ) )->kind, 17 );
    QCOMPARE( findByLabel( candidates, QStringLiteral( "icons" ) )->kind, 19 );
}

void TestRstPathCompletion::ranksCurrentDirectoryAboveNothingAndPrefersMatchingExtensions()
{
    const Query query = makeQuery( QStringLiteral( ".. include:: " ), 14 );
    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( sampleEntries() ) );

    // 디렉터리가 파일보다 높고, include 가 선호하는 문서 확장자가 그다음이다.
    const int directory = findByLabel( candidates, QStringLiteral( "icons" ) )->scoreBias;
    const int document = findByLabel( candidates, QStringLiteral( "intro.rst" ) )->scoreBias;
    const int image = findByLabel( candidates, QStringLiteral( "logo.png" ) )->scoreBias;
    QVERIFY( directory > document );
    QVERIFY( document > image );
}

void TestRstPathCompletion::offersParentDirectory()
{
    const Query query = makeQuery( QStringLiteral( ".. image:: " ), 12 );
    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( sampleEntries() ) );

    const Candidate* up = findByLabel( candidates, QStringLiteral( ".." ) );
    QVERIFY( up != nullptr );
    QCOMPARE( up->insertText, QStringLiteral( "../" ) );
    QVERIFY( up->isDirectory );

    // 항목이 하나도 없으면 ".." 만 덩그러니 띄우지 않는다 (오타로 없는 경로를 친 경우).
    QVERIFY( oneLevelCandidates( query, fakeLister( {} ) ).isEmpty() );
}

void TestRstPathCompletion::docNameSlotStripsSuffixAndStaysInsideSourceRoot()
{
    Query query;
    query.context = mrst::rstcomplete::detectContext(
        QStringLiteral( "   " ), 4, { QStringLiteral( ".. toctree::" ) } );
    query.documentDirectory = QString::fromLatin1( kSourceRoot );   // srcdir 최상단
    query.sourceRoot = QString::fromLatin1( kSourceRoot );

    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( sampleEntries() ) );

    // 확장자를 뗀 docname 이어야 한다.
    QVERIFY( findByLabel( candidates, QStringLiteral( "intro" ) ) != nullptr );
    QCOMPARE( findByLabel( candidates, QStringLiteral( "intro" ) )->insertText,
             QStringLiteral( "intro" ) );
    // 이미지는 문서가 아니다.
    QVERIFY( findByLabel( candidates, QStringLiteral( "logo.png" ) ) == nullptr );
    // 소스 루트 밖으로 나가는 ".." 은 반드시 빌드 경고가 되므로 제안하지 않는다.
    QVERIFY( findByLabel( candidates, QStringLiteral( ".." ) ) == nullptr );
}

void TestRstPathCompletion::escapesSpacesInInsertTextForImageSlot()
{
    const QVector< DirEntry > entries{ { QStringLiteral( "my photo.jpg" ), false } };
    const Query query = makeQuery( QStringLiteral( ".. image:: " ), 12 );
    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( entries ) );

    const Candidate* photo = findByLabel( candidates, QStringLiteral( "my photo.jpg" ) );
    QVERIFY( photo != nullptr );
    QCOMPARE( photo->insertText, QStringLiteral( "my\\ photo.jpg" ) );
}

void TestRstPathCompletion::prependsSpaceWhenArgumentHasNone()
{
    const QVector< DirEntry > entries{ { QStringLiteral( "logo.png" ), false } };
    const Query query = makeQuery( QStringLiteral( ".. image::" ), 11 );
    QVERIFY( query.context.argumentNeedsSpace );

    const QVector< Candidate > candidates = oneLevelCandidates( query, fakeLister( entries ) );
    const Candidate* logo = findByLabel( candidates, QStringLiteral( "logo.png" ) );
    QVERIFY( logo != nullptr );
    QCOMPARE( logo->insertText, QStringLiteral( " logo.png" ) );
}

// ── 전역 퍼지 후보 ─────────────────────────────────────────
//
// 이 기능의 값어치는 대부분 여기서 나온다. 실사용 문서가 쓰는 경로는
// "../../../../Resources/Novel/Pt5/Vol12/LN_P5V12-3.jpg" 같은 것이라 손으로
// 치기가 고약한데, 파일 이름만 알면 여기서 잡힌다.

namespace {

constexpr const char* kIndexRoot = "C:/proj";

const QStringList& sampleIndex()
{
    static const QStringList paths{
        QStringLiteral( "Resources/Novel/Pt5/Vol12/LN_P5V12-3.jpg" ),
        QStringLiteral( "Resources/Novel/Pt1/Vol1/Map01KOR.jpg" ),
        QStringLiteral( "docs/_static/cover.png" ),
        QStringLiteral( "docs/guide/index.rst" ),
        QStringLiteral( "docs/ref/index.rst" ),
        QStringLiteral( "notes.txt" ),
    };
    return paths;
}

}   // namespace

void TestRstPathCompletion::findsFileAnywhereByNameAlone()
{
    const Query query = makeQuery( QStringLiteral( ".. image:: P5V12-3" ), 19 );
    const QVector< Candidate > candidates =
        fuzzyCandidates( query, QString::fromLatin1( kIndexRoot ), sampleIndex() );

    QCOMPARE( candidates.size(), 1 );
    QCOMPARE( candidates.first().label, QStringLiteral( "LN_P5V12-3.jpg" ) );
    // 문서는 C:/proj/docs/guide 에 있다. 소스 루트 밖으로 나가는 경로가 나와야 한다.
    QCOMPARE( candidates.first().insertText,
             QStringLiteral( "../../Resources/Novel/Pt5/Vol12/LN_P5V12-3.jpg" ) );
}

void TestRstPathCompletion::skipsGlobalSearchWhenNameTooShort()
{
    // 접두 없이 전역 목록을 통째로 붙이는 것은 소음이다.
    const Query empty = makeQuery( QStringLiteral( ".. image:: " ), 12 );
    QVERIFY( fuzzyCandidates( empty, QString::fromLatin1( kIndexRoot ), sampleIndex() ).isEmpty() );

    const Query single = makeQuery( QStringLiteral( ".. image:: c" ), 13 );
    QVERIFY( fuzzyCandidates( single, QString::fromLatin1( kIndexRoot ), sampleIndex() ).isEmpty() );
}

void TestRstPathCompletion::globalCandidatesRespectSlotExtensions()
{
    // "index" 는 .rst 만 맞는다. 이미지 자리에서는 하나도 나오면 안 된다.
    const Query image = makeQuery( QStringLiteral( ".. image:: index" ), 17 );
    QVERIFY( fuzzyCandidates( image, QString::fromLatin1( kIndexRoot ), sampleIndex() ).isEmpty() );

    const Query include = makeQuery( QStringLiteral( ".. include:: index" ), 19 );
    QCOMPARE( fuzzyCandidates( include, QString::fromLatin1( kIndexRoot ), sampleIndex() ).size(), 2 );
}

void TestRstPathCompletion::globalCandidatesCarryRelativeDirectoryInDetail()
{
    // 라벨은 파일 이름뿐이라 동명 파일이 여럿이면 이것만이 구분 수단이다.
    const Query query = makeQuery( QStringLiteral( ".. include:: index" ), 19 );
    const QVector< Candidate > candidates =
        fuzzyCandidates( query, QString::fromLatin1( kIndexRoot ), sampleIndex() );

    QStringList details;
    for( const Candidate& candidate : candidates )
        details << candidate.detail;
    details.sort();
    QCOMPARE( details, QStringList( { QStringLiteral( "docs/guide" ), QStringLiteral( "docs/ref" ) } ) );
}

void TestRstPathCompletion::globalCandidatesRankShallowerPathsFirst()
{
    const QStringList index{ QStringLiteral( "a/b/c/d/cover.png" ),
                            QStringLiteral( "cover.png" ) };
    const Query query = makeQuery( QStringLiteral( ".. image:: cover" ), 17 );
    const QVector< Candidate > candidates =
        fuzzyCandidates( query, QString::fromLatin1( kIndexRoot ), index );

    QCOMPARE( candidates.size(), 2 );
    QCOMPARE( candidates.first().detail, QString{} );   // 루트 바로 아래가 먼저
}

void TestRstPathCompletion::docNameSlotRejectsPathsOutsideSourceRoot()
{
    Query query;
    query.context = mrst::rstcomplete::detectContext(
        QStringLiteral( "   Map01" ), 9, { QStringLiteral( ".. toctree::" ) } );
    query.documentDirectory = QString::fromLatin1( kSourceRoot );
    query.sourceRoot = QString::fromLatin1( kSourceRoot );

    // Resources/ 는 소스 루트 밖이다. docname 으로 제안하면 반드시 빌드 경고가 된다.
    QVERIFY( fuzzyCandidates( query, QString::fromLatin1( kIndexRoot ), sampleIndex() ).isEmpty() );
}

void TestRstPathCompletion::mergeKeepsOneLevelFirstAndDropsDuplicates()
{
    Candidate near;
    near.label = QStringLiteral( "cover.png" );
    near.insertText = QStringLiteral( "_static/cover.png" );
    Candidate far;
    far.label = QStringLiteral( "other.png" );
    far.insertText = QStringLiteral( "../x/other.png" );
    Candidate duplicate;
    duplicate.label = QStringLiteral( "cover.png" );
    duplicate.insertText = QStringLiteral( "_static/cover.png" );

    const QVector< Candidate > merged = mergeCandidates( { near }, { duplicate, far } );
    QCOMPARE( merged.size(), 2 );
    QCOMPARE( merged.at( 0 ).insertText, QStringLiteral( "_static/cover.png" ) );
    QCOMPARE( merged.at( 1 ).insertText, QStringLiteral( "../x/other.png" ) );
}

// ── 인덱스 순회 ───────────────────────────────────────────

void TestRstPathCompletion::scanSkipsExcludedDirectoriesAtAnyDepth()
{
    QTemporaryDir temporary;
    QVERIFY( temporary.isValid() );
    const QDir root( temporary.path() );

    // 실사용 워크스페이스는 프로젝트 **안쪽 깊은 곳**에 _build 를 두고 원본
    // 이미지를 통째로 복사해 둔다. 거기까지 걸러야 후보가 오염되지 않는다.
    QVERIFY( root.mkpath( QStringLiteral( "docs/source/_build/preview/_images" ) ) );
    QVERIFY( root.mkpath( QStringLiteral( "docs/source/_static" ) ) );
    QVERIFY( root.mkpath( QStringLiteral( ".git/objects" ) ) );

    const QStringList files{ QStringLiteral( "docs/source/_static/cover.png" ),
                            QStringLiteral( "docs/source/_build/preview/_images/cover.png" ),
                            QStringLiteral( ".git/objects/blob" ) };
    for( const QString& relative : files )
    {
        QFile file( root.absoluteFilePath( relative ) );
        QVERIFY( file.open( QIODevice::WriteOnly ) );
        file.close();
    }

    const QStringList found = mrst::scanPathIndex( temporary.path() );
    QCOMPARE( found, QStringList( { QStringLiteral( "docs/source/_static/cover.png" ) } ) );
}

void TestRstPathCompletion::scanReturnsRootRelativeForwardSlashPaths()
{
    QTemporaryDir temporary;
    QVERIFY( temporary.isValid() );
    const QDir root( temporary.path() );
    QVERIFY( root.mkpath( QStringLiteral( "a/b" ) ) );

    QFile file( root.absoluteFilePath( QStringLiteral( "a/b/c.txt" ) ) );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    file.close();

    const QStringList found = mrst::scanPathIndex( temporary.path() );
    QCOMPARE( found, QStringList( { QStringLiteral( "a/b/c.txt" ) } ) );
}

void TestRstPathCompletion::scanRespectsLimit()
{
    QTemporaryDir temporary;
    QVERIFY( temporary.isValid() );
    const QDir root( temporary.path() );
    for( int index = 0; index < 12; ++index )
    {
        QFile file( root.absoluteFilePath( QStringLiteral( "f%1.txt" ).arg( index ) ) );
        QVERIFY( file.open( QIODevice::WriteOnly ) );
        file.close();
    }

    QCOMPARE( mrst::scanPathIndex( temporary.path(), 5 ).size(), 5 );
}

MRST_REGISTER_TEST( TestRstPathCompletion );

#include "tst_RstPathCompletion.moc"
