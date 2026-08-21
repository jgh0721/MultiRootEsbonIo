#include "TestRunner.hpp"

#include "core/solWorkspaceSearch.hpp"
#include "core/solWorkspaceSession.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace mrst;

namespace {

QStringList lines( const QString& text )
{
    return text.split( QLatin1Char( '\n' ) );
}

void writeFile( const QString& path, const QString& content )
{
    QDir().mkpath( QFileInfo( path ).absolutePath() );
    QFile file( path );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    file.write( content.toUtf8() );
}

}  // namespace

class TestWorkspaceSearch : public QObject
{
    Q_OBJECT

private slots:
    // ── unified diff ──
    void identicalTextProducesNoDiff();
    /// src_cpp 판의 실패 지점. 한 줄만 끼워 넣어도 그 뒤 전부가 변경으로 잡혔다.
    void singleInsertionDoesNotShiftEverything();
    void deletionIsReportedOnce();
    void respectsContextLines();
    void separateChangesProduceSeparateHunks();
    void hunkHeaderLineNumbersAreOneBased();

    // ── 검색 ──
    void findsMatchesWithLineAndColumn();
    void caseSensitivityIsHonored();
    void wholeWordsDoesNotMatchSubstring();
    void skipsBuildDirectories();
    void previewCountsAndDiffsButDoesNotWrite();
    void applyWritesOnlyChangedFiles();

    // ── 세션 ──
    void sessionRoundTrips();
    void sessionWithoutDockLayoutStillLoads();
    void sessionRejectsUnknownSchema();
    void sessionDropsOutOfRangeActiveIndex();
};

void TestWorkspaceSearch::identicalTextProducesNoDiff()
{
    const QStringList text = lines( QStringLiteral( "a\nb\nc\n" ) );
    QVERIFY( unifiedDiffLines( text, text ).isEmpty() );
    QVERIFY( unifiedDiff( QStringLiteral( "a\nb\n" ), QStringLiteral( "a\nb\n" ),
                         QStringLiteral( "x.rst" ) ).isEmpty() );
}

void TestWorkspaceSearch::singleInsertionDoesNotShiftEverything()
{
    const QStringList before = lines( QStringLiteral( "1\n2\n3\n4\n5\n6\n7\n8" ) );
    const QStringList after = lines( QStringLiteral( "1\n2\n삽입\n3\n4\n5\n6\n7\n8" ) );

    const QStringList diff = unifiedDiffLines( before, after, 1 );

    int added = 0;
    int removed = 0;
    for( const QString& line : diff )
    {
        if( line.startsWith( QLatin1Char( '@' ) ) )
            continue;
        if( line.startsWith( QLatin1Char( '+' ) ) )
            ++added;
        else if( line.startsWith( QLatin1Char( '-' ) ) )
            ++removed;
    }

    QCOMPARE( added, 1 );
    QCOMPARE( removed, 0 );
}

void TestWorkspaceSearch::deletionIsReportedOnce()
{
    const QStringList before = lines( QStringLiteral( "1\n2\n3\n4\n5" ) );
    const QStringList after = lines( QStringLiteral( "1\n2\n4\n5" ) );

    const QStringList diff = unifiedDiffLines( before, after, 1 );
    QCOMPARE( diff.count( QStringLiteral( "-3" ) ), 1 );
    for( const QString& line : diff )
        QVERIFY2( !line.startsWith( QLatin1Char( '+' ) ), qPrintable( line ) );
}

void TestWorkspaceSearch::respectsContextLines()
{
    QStringList before;
    for( int index = 1; index <= 40; ++index )
        before << QString::number( index );
    QStringList after = before;
    after[ 20 ] = QStringLiteral( "바뀜" );

    const QStringList narrow = unifiedDiffLines( before, after, 1 );
    const QStringList wide = unifiedDiffLines( before, after, 5 );

    QVERIFY2( narrow.size() < wide.size(), "contextLines 가 실제로 반영돼야 한다" );
    // 컨텍스트 1 이면 hunk 는 머리말 + (앞 1 · -1 · +1 · 뒤 1) 정도로 짧다.
    QVERIFY2( narrow.size() <= 8, qPrintable( narrow.join( QLatin1Char( '/' ) ) ) );
}

void TestWorkspaceSearch::separateChangesProduceSeparateHunks()
{
    QStringList before;
    for( int index = 1; index <= 60; ++index )
        before << QString::number( index );
    QStringList after = before;
    after[ 5 ] = QStringLiteral( "앞쪽 변경" );
    after[ 50 ] = QStringLiteral( "뒤쪽 변경" );

    const QStringList diff = unifiedDiffLines( before, after, 2 );

    int hunks = 0;
    for( const QString& line : diff )
        hunks += line.startsWith( QStringLiteral( "@@" ) ) ? 1 : 0;
    QCOMPARE( hunks, 2 );
}

void TestWorkspaceSearch::hunkHeaderLineNumbersAreOneBased()
{
    const QStringList before = lines( QStringLiteral( "a\nb\nc" ) );
    const QStringList after = lines( QStringLiteral( "a\nB\nc" ) );

    const QStringList diff = unifiedDiffLines( before, after, 0 );
    QVERIFY( !diff.isEmpty() );
    // 2번째 줄만 바뀌었다.
    QCOMPARE( diff.first(), QStringLiteral( "@@ -2,1 +2,1 @@" ) );
}

void TestWorkspaceSearch::findsMatchesWithLineAndColumn()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    writeFile( QDir( temp.path() ).filePath( QStringLiteral( "a.rst" ) ),
              QStringLiteral( "제목\n====\n\n여기에 esbonio 가 있다\n" ) );

    const QVector< SearchMatch > matches = findInFiles( temp.path(), QStringLiteral( "esbonio" ) );
    QCOMPARE( matches.size(), 1 );
    QCOMPARE( matches.first().line, 4 );
    QCOMPARE( matches.first().column, 5 );
}

void TestWorkspaceSearch::caseSensitivityIsHonored()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    writeFile( QDir( temp.path() ).filePath( QStringLiteral( "a.rst" ) ),
              QStringLiteral( "Sphinx 와 sphinx\n" ) );

    SearchOptions sensitive;
    sensitive.caseSensitive = true;
    QCOMPARE( findInFiles( temp.path(), QStringLiteral( "Sphinx" ), sensitive ).size(), 1 );
    QCOMPARE( findInFiles( temp.path(), QStringLiteral( "Sphinx" ) ).size(), 2 );
}

void TestWorkspaceSearch::wholeWordsDoesNotMatchSubstring()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    writeFile( QDir( temp.path() ).filePath( QStringLiteral( "a.rst" ) ),
              QStringLiteral( "doc docs document\n" ) );

    SearchOptions whole;
    whole.wholeWords = true;
    QCOMPARE( findInFiles( temp.path(), QStringLiteral( "doc" ), whole ).size(), 1 );
    QCOMPARE( findInFiles( temp.path(), QStringLiteral( "doc" ) ).size(), 3 );
}

void TestWorkspaceSearch::skipsBuildDirectories()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    writeFile( QDir( temp.path() ).filePath( QStringLiteral( "a.rst" ) ), QStringLiteral( "찾을것\n" ) );
    writeFile( QDir( temp.path() ).filePath( QStringLiteral( "_build/html/b.rst" ) ),
              QStringLiteral( "찾을것\n" ) );
    writeFile( QDir( temp.path() ).filePath( QStringLiteral( ".venv/lib/c.rst" ) ),
              QStringLiteral( "찾을것\n" ) );

    QCOMPARE( findInFiles( temp.path(), QStringLiteral( "찾을것" ) ).size(), 1 );
}

void TestWorkspaceSearch::previewCountsAndDiffsButDoesNotWrite()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    const QString path = QDir( temp.path() ).filePath( QStringLiteral( "a.rst" ) );
    const QString original = QStringLiteral( "옛말 하나\n그대로\n옛말 둘\n" );
    writeFile( path, original );

    const QVector< ReplacePreview > previews =
        previewReplaceInFiles( temp.path(), QStringLiteral( "옛말" ), QStringLiteral( "새말" ) );

    QCOMPARE( previews.size(), 1 );
    QCOMPARE( previews.first().replacements, 2 );
    QVERIFY( previews.first().diff.contains( QStringLiteral( "+새말 하나" ) ) );

    QFile file( path );
    QVERIFY( file.open( QIODevice::ReadOnly ) );
    QCOMPARE( QString::fromUtf8( file.readAll() ), original );
}

void TestWorkspaceSearch::applyWritesOnlyChangedFiles()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    const QString hit = QDir( temp.path() ).filePath( QStringLiteral( "hit.rst" ) );
    const QString miss = QDir( temp.path() ).filePath( QStringLiteral( "miss.rst" ) );
    writeFile( hit, QStringLiteral( "옛말\n" ) );
    writeFile( miss, QStringLiteral( "무관\n" ) );

    const QStringList changed =
        applyReplaceInFiles( { hit, miss }, QStringLiteral( "옛말" ), QStringLiteral( "새말" ) );

    QCOMPARE( changed.size(), 1 );
    QCOMPARE( changed.first(), hit );

    QFile file( hit );
    QVERIFY( file.open( QIODevice::ReadOnly ) );
    QCOMPARE( QString::fromUtf8( file.readAll() ), QStringLiteral( "새말\n" ) );
}

void TestWorkspaceSearch::sessionRoundTrips()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    WorkspaceSession session;
    session.workspaceRoot = temp.path();
    session.documents = { { QStringLiteral( "C:/a.rst" ), 12, 3, 8 },
                         { QStringLiteral( "C:/b.rst" ), 1, 1, 1 } };
    session.activeIndex = 1;
    session.previewSplitterSizes = { 700, 200 };
    session.dockLayout = QStringLiteral( "eJxLyU/OTs0rzs8rzi/OL0nNzQMAWkoJhA==" );

    QVERIFY( saveWorkspaceSession( session ) );
    QVERIFY( QFile::exists( sessionFilePath( temp.path() ) ) );

    const WorkspaceSession restored = loadWorkspaceSession( temp.path() );
    QCOMPARE( restored.documents.size(), 2 );
    QCOMPARE( restored.documents.first().caretLine, 12 );
    QCOMPARE( restored.documents.first().firstVisibleLine, 8 );
    QCOMPARE( restored.activeIndex, 1 );
    QCOMPARE( restored.previewSplitterSizes, QList< int >( { 700, 200 } ) );
    QCOMPARE( restored.dockLayout, session.dockLayout );
}

void TestWorkspaceSearch::sessionWithoutDockLayoutStillLoads()
{
    // 이 버전보다 먼저 만들어진 파일에는 dockLayout 이 없다. 스키마를 올리지
    // 않은 것이 그래서다 — 올렸다면 아래 세션이 통째로 버려지고 사용자가 열어
    // 둔 탭을 잃는다. 배치만 기본값으로 시작하면 된다.
    const QJsonObject old{
        { QStringLiteral( "schema" ), 1 },
        { QStringLiteral( "workspaceRoot" ), QStringLiteral( "C:/w" ) },
        { QStringLiteral( "documents" ),
         QJsonArray{ QJsonObject{ { QStringLiteral( "path" ), QStringLiteral( "C:/a.rst" ) },
                                  { QStringLiteral( "caretLine" ), 7 } } } },
        { QStringLiteral( "activeIndex" ), 0 },
    };

    const WorkspaceSession restored = sessionFromJson( old );
    QCOMPARE( restored.documents.size(), 1 );
    QCOMPARE( restored.documents.first().caretLine, 7 );
    QCOMPARE( restored.activeIndex, 0 );
    QVERIFY( restored.dockLayout.isEmpty() );
}

void TestWorkspaceSearch::sessionRejectsUnknownSchema()
{
    // 미래 버전이 쓴 파일을 옛 코드가 반쯤 읽어 창이 이상해지는 것보다,
    // 복원을 포기하는 편이 낫다.
    const QJsonObject future{ { QStringLiteral( "schema" ), 99 },
                             { QStringLiteral( "workspaceRoot" ), QStringLiteral( "C:/w" ) } };
    QVERIFY( sessionFromJson( future ).isEmpty() );
}

void TestWorkspaceSearch::sessionDropsOutOfRangeActiveIndex()
{
    WorkspaceSession session;
    session.workspaceRoot = QStringLiteral( "C:/w" );
    session.documents = { { QStringLiteral( "C:/a.rst" ), 1, 1, 1 } };
    session.activeIndex = 5;

    QCOMPARE( sessionFromJson( sessionToJson( session ) ).activeIndex, -1 );
}

MRST_REGISTER_TEST( TestWorkspaceSearch );

#include "tst_WorkspaceSearch.moc"
