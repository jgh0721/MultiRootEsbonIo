#include "TestRunner.hpp"

#include "core/solQuickOpenSearch.hpp"

#include <QTest>

#include <memory>
#include <utility>

using namespace mrst;

namespace {

QStringList resultPaths( const QVector< QuickOpenMatch >& matches )
{
    QStringList paths;
    paths.reserve( matches.size() );
    for( const QuickOpenMatch& match : matches )
        paths.push_back( match.relativePath );
    return paths;
}

QStringList resultPaths( const QuickOpenRankedMatches& matches )
{
    QStringList paths;
    paths.reserve( matches.size() );
    for( const QuickOpenRankedMatch& match : matches )
        paths.push_back( match.match.relativePath );
    return paths;
}

}   // namespace

class TestQuickOpenSearch : public QObject
{
    Q_OBJECT

private slots:
    void normalizesBothSeparators();
    void ranksFileNameKindsBeforePathMatches();
    void pathQueryMatchesSegmentsInOrder();
    void slashAndBackslashQueriesAreEquivalent();
    void trailingSlashOnlyReturnsDirectoryDescendants();
    void emptyQueryPutsRecentFilesBeforeAlphabeticalFiles();
    void returnsHighlightPositionsForFileNameAndPath();
    void bareQueryCanMatchDirectoryPath();
    void tiedMatchesHaveDeterministicAlphabeticalOrder();
    void immutableChunksMatchFlatRanking();
    void incrementalChunkMergeMatchesFullRanking();
    void returnsEveryMatchWithoutPaginationLimit();
    void cancellationReturnsNoPartialResults();
    void enforcesThePublicQueryLengthLimit();
    void rejectsFuzzyMatchesBeyondTheCellBudget();
};

void TestQuickOpenSearch::normalizesBothSeparators()
{
    QCOMPARE( normalizeQuickOpenPath( QStringLiteral( R"(./src\\core/../uis//Main.cpp/)" ) ),
              QStringLiteral( "src/uis/Main.cpp" ) );
    QVERIFY( normalizeQuickOpenPath( QString{} ).isEmpty() );
}

void TestQuickOpenSearch::ranksFileNameKindsBeforePathMatches()
{
    const QStringList paths{
        QStringLiteral( "main/archive/readme.txt" ),
        QStringLiteral( "misc/MegaApplicationIndex.md" ),
        QStringLiteral( "old/my-main-file.cpp" ),
        QStringLiteral( "src/MainWindow.cpp" ),
        QStringLiteral( "docs/main" ),
    };

    const QVector< QuickOpenMatch > matches =
        rankQuickOpenFiles( paths, QStringLiteral( "MAIN" ) );

    QCOMPARE( resultPaths( matches ),
              QStringList( { QStringLiteral( "docs/main" ),
                             QStringLiteral( "src/MainWindow.cpp" ),
                             QStringLiteral( "old/my-main-file.cpp" ),
                             QStringLiteral( "misc/MegaApplicationIndex.md" ),
                             QStringLiteral( "main/archive/readme.txt" ) } ) );
}

void TestQuickOpenSearch::pathQueryMatchesSegmentsInOrder()
{
    const QStringList paths{
        QStringLiteral( "uis/src/MainWindow.cpp" ),
        QStringLiteral( "src/uis/MainWindow.cpp" ),
        QStringLiteral( "source/src/tools/ui/MainWindow.cpp" ),
        QStringLiteral( "src/uis/Other.cpp" ),
    };

    const QVector< QuickOpenMatch > matches =
        rankQuickOpenFiles( paths, QStringLiteral( "sr/ui/mainw" ) );

    QCOMPARE( resultPaths( matches ),
              QStringList( { QStringLiteral( "src/uis/MainWindow.cpp" ),
                             QStringLiteral( "source/src/tools/ui/MainWindow.cpp" ) } ) );
}

void TestQuickOpenSearch::slashAndBackslashQueriesAreEquivalent()
{
    const QStringList paths{
        QStringLiteral( R"(src\uis\MainWindow.cpp)" ),
        QStringLiteral( "docs/MainWindow.cpp" ),
    };

    const QVector< QuickOpenMatch > slash =
        rankQuickOpenFiles( paths, QStringLiteral( "src/ui/main" ) );
    const QVector< QuickOpenMatch > backslash =
        rankQuickOpenFiles( paths, QStringLiteral( R"(src\ui\main)" ) );

    QCOMPARE( resultPaths( slash ), resultPaths( backslash ) );
    QCOMPARE( slash.first().fileNameMatchedPositions,
              backslash.first().fileNameMatchedPositions );
    QCOMPARE( slash.first().pathMatchedPositions, backslash.first().pathMatchedPositions );
}

void TestQuickOpenSearch::trailingSlashOnlyReturnsDirectoryDescendants()
{
    const QStringList paths{
        QStringLiteral( "docs/api/v1/reference.rst" ),
        QStringLiteral( "docs/guide/api.rst" ),
        QStringLiteral( "api/docs/index.rst" ),
        QStringLiteral( "docs/api/index.rst" ),
    };

    const QVector< QuickOpenMatch > matches =
        rankQuickOpenFiles( paths, QStringLiteral( R"(DOCS\API\)" ) );

    QCOMPARE( resultPaths( matches ),
              QStringList( { QStringLiteral( "docs/api/index.rst" ),
                             QStringLiteral( "docs/api/v1/reference.rst" ) } ) );
    QVERIFY( matches.first().fileNameMatchedPositions.isEmpty() );
}

void TestQuickOpenSearch::emptyQueryPutsRecentFilesBeforeAlphabeticalFiles()
{
    const QStringList paths{
        QStringLiteral( "zeta.rst" ),
        QStringLiteral( R"(docs\b.rst)" ),
        QStringLiteral( "alpha.rst" ),
        QStringLiteral( "docs/A.rst" ),
    };
    const QStringList recent{
        QStringLiteral( R"(DOCS\B.RST)" ),
        QStringLiteral( "missing.rst" ),
    };

    const QVector< QuickOpenMatch > matches = rankQuickOpenFiles( paths, {}, recent );

    QCOMPARE( resultPaths( matches ),
              QStringList( { QStringLiteral( "docs/b.rst" ),
                             QStringLiteral( "alpha.rst" ),
                             QStringLiteral( "docs/A.rst" ),
                             QStringLiteral( "zeta.rst" ) } ) );
    for( const QuickOpenMatch& match : matches )
    {
        QVERIFY( match.fileNameMatchedPositions.isEmpty() );
        QVERIFY( match.pathMatchedPositions.isEmpty() );
    }
}

void TestQuickOpenSearch::returnsHighlightPositionsForFileNameAndPath()
{
    const QVector< QuickOpenMatch > matches = rankQuickOpenFiles(
        { QStringLiteral( "src/uis/MainWindow.cpp" ) }, QStringLiteral( "sr/ui/MW" ) );

    QCOMPARE( matches.size(), 1 );
    QCOMPARE( matches.first().fileName, QStringLiteral( "MainWindow.cpp" ) );
    QCOMPARE( matches.first().directoryPath, QStringLiteral( "src/uis" ) );
    QCOMPARE( matches.first().fileNameMatchedPositions, QVector< int >( { 0, 4 } ) );
    QCOMPARE( matches.first().pathMatchedPositions, QVector< int >( { 0, 1, 4, 5 } ) );
}

void TestQuickOpenSearch::bareQueryCanMatchDirectoryPath()
{
    const QStringList paths{
        QStringLiteral( "docs/guides/index.rst" ),
        QStringLiteral( "src/guides.txt" ),
        QStringLiteral( "docs/reference/index.rst" ),
    };

    const QVector< QuickOpenMatch > matches =
        rankQuickOpenFiles( paths, QStringLiteral( "guides" ) );

    QCOMPARE( resultPaths( matches ),
              QStringList( { QStringLiteral( "src/guides.txt" ),
                             QStringLiteral( "docs/guides/index.rst" ) } ) );
    QCOMPARE( matches.at( 1 ).pathMatchedPositions,
              QVector< int >( { 5, 6, 7, 8, 9, 10 } ) );
}

void TestQuickOpenSearch::tiedMatchesHaveDeterministicAlphabeticalOrder()
{
    const QStringList forward{ QStringLiteral( "ca.txt" ), QStringLiteral( "ba.txt" ) };
    const QStringList reverse{ QStringLiteral( "ba.txt" ), QStringLiteral( "ca.txt" ) };
    const QStringList expected{ QStringLiteral( "ba.txt" ), QStringLiteral( "ca.txt" ) };

    QCOMPARE( resultPaths( rankQuickOpenFiles( forward, QStringLiteral( "a" ) ) ), expected );
    QCOMPARE( resultPaths( rankQuickOpenFiles( reverse, QStringLiteral( "a" ) ) ), expected );
}

void TestQuickOpenSearch::immutableChunksMatchFlatRanking()
{
    const QStringList first{
        QStringLiteral( "docs/index.rst" ),
        QStringLiteral( "src/uis/MainWindow.cpp" ),
    };
    const QStringList second{
        QStringLiteral( R"(src\core\solQuickOpenSearch.cpp)" ),
        QStringLiteral( "tests/tst_QuickOpenSearch.cpp" ),
    };
    const QStringList flat = first + second;
    const QuickOpenPathChunks chunks{
        std::make_shared< const QStringList >( first ),
        {},
        std::make_shared< const QStringList >( second ),
    };
    const QStringList recent{ QStringLiteral( "tests/tst_QuickOpenSearch.cpp" ) };

    for( const QString& query : { QString{}, QStringLiteral( "quick" ),
                                  QStringLiteral( R"(src\ui\main)" ) } )
    {
        const QVector<QuickOpenMatch> expected = rankQuickOpenFiles( flat, query, recent );
        const QVector<QuickOpenMatch> actual =
            rankQuickOpenFileChunks( chunks, query, recent );
        QCOMPARE( resultPaths( actual ), resultPaths( expected ) );
        QCOMPARE( actual.size(), expected.size() );
        for( qsizetype index = 0; index < actual.size(); ++index )
        {
            QCOMPARE( actual.at( index ).fileNameMatchedPositions,
                      expected.at( index ).fileNameMatchedPositions );
            QCOMPARE( actual.at( index ).pathMatchedPositions,
                      expected.at( index ).pathMatchedPositions );
        }
    }
}

void TestQuickOpenSearch::incrementalChunkMergeMatchesFullRanking()
{
    const QStringList first{
        QStringLiteral( "docs/zeta.rst" ),
        QStringLiteral( "src/uis/MainWindow.cpp" ),
        QStringLiteral( "tests/tst_MainWindow.cpp" ),
    };
    const QStringList second{
        QStringLiteral( "docs/alpha.rst" ),
        QStringLiteral( "src/core/solQuickOpenSearch.cpp" ),
        QStringLiteral( "tests/tst_QuickOpenSearch.cpp" ),
    };
    const QStringList recent{ QStringLiteral( "tests/tst_QuickOpenSearch.cpp" ),
                              QStringLiteral( "docs/zeta.rst" ) };
    const QuickOpenPathChunks firstChunk{
        std::make_shared< const QStringList >( first ),
    };
    const QuickOpenPathChunks secondChunk{
        std::make_shared< const QStringList >( second ),
    };
    const QuickOpenPathChunks allChunks{
        firstChunk.first(), secondChunk.first(),
    };

    for( const QString& query : { QString{}, QStringLiteral( "quick" ),
                                  QStringLiteral( R"(src\ui\main)" ) } )
    {
        const QuickOpenRankedMatches initial = rankQuickOpenFileChunksDetailed(
            firstChunk, query, recent, 0 );
        QuickOpenRankedMatches added = rankQuickOpenFileChunksDetailed(
            secondChunk, query, recent, first.size() );
        const QuickOpenRankedMatches merged = mergeQuickOpenRankedMatches(
            initial, std::move( added ), query );
        const QuickOpenRankedMatches expected = rankQuickOpenFileChunksDetailed(
            allChunks, query, recent, 0 );

        QCOMPARE( resultPaths( merged ), resultPaths( expected ) );
    }
}

void TestQuickOpenSearch::returnsEveryMatchWithoutPaginationLimit()
{
    QStringList paths;
    for( int index = 0; index < 100'000; ++index )
        paths.push_back( QStringLiteral( "bulk/file%1.rst" ).arg( index, 6, 10, QLatin1Char( '0' ) ) );

    QCOMPARE( rankQuickOpenFiles( paths, QStringLiteral( "file" ) ).size(), 100'000 );
}

void TestQuickOpenSearch::cancellationReturnsNoPartialResults()
{
    QStringList paths;
    for( int index = 0; index < 10'000; ++index )
        paths.push_back( QStringLiteral( "bulk/file%1.rst" ).arg( index ) );

    std::stop_source stopSource;
    stopSource.request_stop();
    QVERIFY( rankQuickOpenFiles( paths, QStringLiteral( "file" ), {},
                                 stopSource.get_token() ).isEmpty() );
}

void TestQuickOpenSearch::enforcesThePublicQueryLengthLimit()
{
    const QString allowedQuery( kQuickOpenMaximumQueryLength, QLatin1Char( 'a' ) );
    const QString rejectedQuery( kQuickOpenMaximumQueryLength + 1, QLatin1Char( 'a' ) );

    QCOMPARE( rankQuickOpenFiles( { allowedQuery }, allowedQuery ).size(), 1 );
    QVERIFY( rankQuickOpenFiles( { allowedQuery }, rejectedQuery ).isEmpty() );
}

void TestQuickOpenSearch::rejectsFuzzyMatchesBeyondTheCellBudget()
{
    const QString query( kQuickOpenMaximumQueryLength, QLatin1Char( 'a' ) );
    const QString oversizedCandidate( 15'626, QLatin1Char( 'a' ) );

    QVERIFY( rankQuickOpenFiles( { oversizedCandidate }, query ).isEmpty() );
}

MRST_REGISTER_TEST( TestQuickOpenSearch );

#include "tst_QuickOpenSearch.moc"
