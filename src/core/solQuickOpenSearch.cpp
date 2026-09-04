#include "stdafx.h"
#include "core/solQuickOpenSearch.hpp"

#include <QHash>

#include <algorithm>
#include <limits>
#include <utility>

namespace mrst {
namespace {

constexpr int kNoScore = (std::numeric_limits< int >::min)() / 4;
constexpr int kNotRecent = (std::numeric_limits< int >::max)();
constexpr qsizetype kMaximumFuzzyCells = 4'000'000;

enum class MatchTier
{
    ExactFileName,
    FileNamePrefixOrBoundary,
    FileNameFuzzy,
    Path,
};

struct FuzzyHit
{
    bool                                matched = false;
    int                                 score = kNoScore;
    QVector< int >                      positions;
};

struct FileNameHit
{
    bool                                matched = false;
    MatchTier                           tier = MatchTier::FileNameFuzzy;
    int                                 score = kNoScore;
    QVector< int >                      positions;
};

struct PathHit
{
    bool                                matched = false;
    int                                 score = kNoScore;
    QVector< int >                      positions;
};

using RankedMatch = QuickOpenRankedMatch;

bool sameCharacter( const char16_t left, const char16_t right )
{
    return QChar::toCaseFolded( left ) == QChar::toCaseFolded( right );
}

bool isBoundary( const QString& text, const int position )
{
    if( position <= 0 )
        return true;

    const char16_t previous = text.at( position - 1 ).unicode();
    const char16_t current = text.at( position ).unicode();
    if( !QChar::isLetterOrNumber( previous ) )
        return true;
    if( QChar::isLower( previous ) && QChar::isUpper( current ) )
        return true;
    return QChar::isDigit( previous ) != QChar::isDigit( current );
}

int characterScore( const QString& candidate, const int position )
{
    int score = 100;
    if( position == 0 )
        score += 45;
    else if( isBoundary( candidate, position ) )
        score += 35;
    return score;
}

/// 가장 점수가 높은 부분수열을 찾는다.
///
/// solRstOfflineCompletions 의 매처는 첫 번째 가능한 부분수열을 택한다. 빠른
/// 열기는 같은 글자가 여러 번 있는 파일에서 연속·경계 일치를 선택해야 하고
/// 경로 구간에도 같은 계산을 써야 하므로 이 작은 순수 매처를 독립적으로 둔다.
FuzzyHit fuzzyMatch( const QString& pattern, const QString& candidate )
{
    FuzzyHit result;
    if( pattern.isEmpty() )
    {
        result.matched = true;
        result.score = 0;
        return result;
    }
    if( pattern.size() > candidate.size() )
        return result;

    const qsizetype patternLength = pattern.size();
    const qsizetype candidateLength = candidate.size();
    if( patternLength > kQuickOpenMaximumQueryLength
        || candidateLength > (std::numeric_limits<int>::max)()
        || ( candidateLength > 0
             && patternLength > kMaximumFuzzyCells / candidateLength ) )
    {
        return result;
    }

    const int patternSize = static_cast<int>( patternLength );
    const int candidateSize = static_cast<int>( candidateLength );
    QVector< int > previousScores( candidateSize, kNoScore );
    QVector< int > currentScores( candidateSize, kNoScore );
    // 각 행의 선택된 직전 위치만 남기면 점수 행은 두 개로 충분하다.
    QVector< int > predecessors( patternLength * candidateLength, -1 );

    for( int position = 0; position < candidateSize; ++position )
    {
        if( sameCharacter( pattern.at( 0 ).unicode(), candidate.at( position ).unicode() ) )
            previousScores[ position ] = characterScore( candidate, position ) - position * 2;
    }

    for( int patternIndex = 1; patternIndex < patternSize; ++patternIndex )
    {
        std::fill( currentScores.begin(), currentScores.end(), kNoScore );
        int bestGapValue = kNoScore;
        int bestGapPosition = -1;

        for( int position = 0; position < candidateSize; ++position )
        {
            // gap 전이는 prev[j] - 4 * (position - j - 1) 이다. 따라서
            // prev[j] + 4*j 의 누적 최댓값을 들고 가면 행 하나가 선형이다.
            const int newlyAvailable = position - 2;
            if( newlyAvailable >= 0 && previousScores.at( newlyAvailable ) != kNoScore )
            {
                const int value = previousScores.at( newlyAvailable ) + newlyAvailable * 4;
                if( value > bestGapValue )
                {
                    bestGapValue = value;
                    bestGapPosition = newlyAvailable;
                }
            }

            if( !sameCharacter( pattern.at( patternIndex ).unicode(),
                                candidate.at( position ).unicode() ) )
                continue;

            int score = kNoScore;
            int before = -1;
            if( bestGapPosition >= 0 )
            {
                score = bestGapValue + characterScore( candidate, position )
                        - ( position - 1 ) * 4;
                before = bestGapPosition;
            }

            // 기존 순회와 같은 tie-break: 같은 점수면 먼저 본 gap 쪽을 남긴다.
            if( position > 0 && previousScores.at( position - 1 ) != kNoScore )
            {
                const int contiguous = previousScores.at( position - 1 )
                                       + characterScore( candidate, position ) + 60;
                if( contiguous > score )
                {
                    score = contiguous;
                    before = position - 1;
                }
            }

            currentScores[ position ] = score;
            predecessors[ patternIndex * candidateSize + position ] = before;
        }
        previousScores.swap( currentScores );
    }

    int bestPosition = -1;
    int bestScore = kNoScore;
    for( int position = 0; position < candidateSize; ++position )
    {
        const int score = previousScores.at( position );
        if( score > bestScore )
        {
            bestScore = score;
            bestPosition = position;
        }
    }
    if( bestPosition < 0 )
        return result;

    result.matched = true;
    result.score = bestScore - candidateSize;
    result.positions.resize( patternSize );
    int position = bestPosition;
    for( int patternIndex = patternSize - 1; patternIndex >= 0; --patternIndex )
    {
        result.positions[ patternIndex ] = position;
        position = predecessors.at( patternIndex * candidateSize + position );
    }
    return result;
}

bool contiguousMatchAt( const QString& pattern, const QString& candidate, const int start )
{
    if( start < 0 || start + pattern.size() > candidate.size() )
        return false;
    for( int index = 0; index < pattern.size(); ++index )
    {
        if( !sameCharacter( pattern.at( index ).unicode(),
                            candidate.at( start + index ).unicode() ) )
            return false;
    }
    return true;
}

QVector< int > contiguousPositions( const int start, const int length )
{
    QVector< int > positions;
    positions.reserve( length );
    for( int index = 0; index < length; ++index )
        positions.push_back( start + index );
    return positions;
}

int boundarySubstring( const QString& pattern, const QString& candidate )
{
    const int lastStart = static_cast< int >( candidate.size() - pattern.size() );
    for( int start = 0; start <= lastStart; ++start )
    {
        if( isBoundary( candidate, start ) && contiguousMatchAt( pattern, candidate, start ) )
            return start;
    }
    return -1;
}

FileNameHit matchFileName( const QString& pattern, const QString& fileName )
{
    FileNameHit result;
    const FuzzyHit fuzzy = fuzzyMatch( pattern, fileName );
    if( !fuzzy.matched )
        return result;

    result.matched = true;
    result.score = fuzzy.score;
    result.positions = fuzzy.positions;

    if( pattern.size() == fileName.size() && contiguousMatchAt( pattern, fileName, 0 ) )
    {
        result.tier = MatchTier::ExactFileName;
        result.score += 10'000;
        result.positions = contiguousPositions( 0, static_cast< int >( pattern.size() ) );
        return result;
    }

    if( contiguousMatchAt( pattern, fileName, 0 ) )
    {
        result.tier = MatchTier::FileNamePrefixOrBoundary;
        result.score += 5'000;
        result.positions = contiguousPositions( 0, static_cast< int >( pattern.size() ) );
        return result;
    }

    const int boundary = boundarySubstring( pattern, fileName );
    if( boundary >= 0 )
    {
        result.tier = MatchTier::FileNamePrefixOrBoundary;
        result.score += 4'500 - boundary;
        result.positions = contiguousPositions( boundary, static_cast< int >( pattern.size() ) );
        return result;
    }

    result.tier = MatchTier::FileNameFuzzy;
    return result;
}

int segmentScore( const QString& pattern, const QString& candidate, const FuzzyHit& fuzzy )
{
    if( pattern.size() == candidate.size() && contiguousMatchAt( pattern, candidate, 0 ) )
        return 800 + fuzzy.score;
    if( contiguousMatchAt( pattern, candidate, 0 ) )
        return 700 + fuzzy.score;
    if( boundarySubstring( pattern, candidate ) >= 0 )
        return 550 + fuzzy.score;
    return 350 + fuzzy.score;
}

bool positionsBefore( const QVector< int >& left, const QVector< int >& right )
{
    return std::lexicographical_compare( left.cbegin(), left.cend(), right.cbegin(), right.cend() );
}

struct DirectoryState
{
    bool                                valid = false;
    int                                 score = kNoScore;
    QVector< int >                      positions;
};

PathHit matchDirectorySegments( const QStringList& patterns, const QStringList& directories )
{
    PathHit result;
    if( patterns.isEmpty() )
    {
        result.matched = true;
        result.score = 0;
        return result;
    }
    if( patterns.size() > directories.size() )
        return result;

    QVector< int > starts;
    starts.reserve( directories.size() );
    int nextStart = 0;
    for( const QString& directory : directories )
    {
        starts.push_back( nextStart );
        nextStart += static_cast< int >( directory.size() ) + 1;
    }

    QVector< DirectoryState > previousStates( directories.size() );
    for( int patternIndex = 0; patternIndex < patterns.size(); ++patternIndex )
    {
        QVector< DirectoryState > states( directories.size() );
        for( int directoryIndex = 0; directoryIndex < directories.size(); ++directoryIndex )
        {
            const FuzzyHit fuzzy = fuzzyMatch( patterns.at( patternIndex ),
                                               directories.at( directoryIndex ) );
            if( !fuzzy.matched )
                continue;

            QVector< int > localPositions;
            localPositions.reserve( fuzzy.positions.size() );
            for( const int position : fuzzy.positions )
                localPositions.push_back( starts.at( directoryIndex ) + position );

            if( patternIndex == 0 )
            {
                states[ directoryIndex ].valid = true;
                states[ directoryIndex ].score =
                    segmentScore( patterns.at( patternIndex ), directories.at( directoryIndex ), fuzzy )
                    - directoryIndex * 40;
                states[ directoryIndex ].positions = std::move( localPositions );
                continue;
            }

            for( int before = 0; before < directoryIndex; ++before )
            {
                if( !previousStates.at( before ).valid )
                    continue;

                const int score = previousStates.at( before ).score
                                  + segmentScore( patterns.at( patternIndex ),
                                                  directories.at( directoryIndex ), fuzzy )
                                  - ( directoryIndex - before - 1 ) * 40;
                QVector< int > positions = previousStates.at( before ).positions;
                positions += localPositions;

                DirectoryState& state = states[ directoryIndex ];
                if( !state.valid || score > state.score
                    || ( score == state.score && positionsBefore( positions, state.positions ) ) )
                {
                    state.valid = true;
                    state.score = score;
                    state.positions = std::move( positions );
                }
            }
        }
        previousStates = std::move( states );
    }

    for( const DirectoryState& state : std::as_const( previousStates ) )
    {
        if( !state.valid )
            continue;
        if( !result.matched || state.score > result.score
            || ( state.score == result.score && positionsBefore( state.positions, result.positions ) ) )
        {
            result.matched = true;
            result.score = state.score;
            result.positions = state.positions;
        }
    }
    return result;
}

int alphabeticalCompare( const RankedMatch& left, const RankedMatch& right )
{
    int order = QString::compare( left.match.relativePath, right.match.relativePath,
                                  Qt::CaseInsensitive );
    if( order == 0 )
        order = QString::compare( left.match.relativePath, right.match.relativePath,
                                  Qt::CaseSensitive );
    if( order == 0 )
    {
        if( left.inputOrder < right.inputOrder )
            return -1;
        if( left.inputOrder > right.inputOrder )
            return 1;
    }
    return order;
}

/// std::sort 한 번에 전체를 맡기면 비교가 끝날 때까지 stop_token을 볼 수 없다.
/// 작은 run을 정렬한 뒤 직접 병합하면 run 사이와 병합 도중에도 최신 검색으로
/// 빠르게 넘길 수 있고, 최종 순서는 같은 strict ordering을 그대로 따른다.
template< typename Less >
bool cancellableSort( QVector< RankedMatch >& items, Less less,
                      const std::stop_token stopToken )
{
    constexpr qsizetype kRunSize = 2'048;
    constexpr qsizetype kCancelCheckStride = 1'024;
    if( stopToken.stop_requested() )
        return false;

    for( qsizetype start = 0; start < items.size(); start += kRunSize )
    {
        if( stopToken.stop_requested() )
            return false;
        const qsizetype end = (std::min)( items.size(), start + kRunSize );
        std::sort( items.begin() + start, items.begin() + end, less );
    }

    QVector<RankedMatch> scratch;
    scratch.resize( items.size() );
    bool sourceIsItems = true;
    for( qsizetype width = kRunSize; width < items.size(); )
    {
        const qsizetype pairWidth = width > items.size() - width
                                        ? items.size()
                                        : width * 2;
        QVector<RankedMatch>& source = sourceIsItems ? items : scratch;
        QVector<RankedMatch>& target = sourceIsItems ? scratch : items;

        for( qsizetype start = 0; start < items.size(); start += pairWidth )
        {
            qsizetype left = start;
            const qsizetype middle = (std::min)( items.size(), start + width );
            qsizetype right = middle;
            const qsizetype end = (std::min)( items.size(), start + pairWidth );
            qsizetype output = start;

            while( left < middle || right < end )
            {
                if( ( output - start ) % kCancelCheckStride == 0
                    && stopToken.stop_requested() )
                {
                    return false;
                }
                if( right >= end
                    || ( left < middle && less( source.at( left ), source.at( right ) ) ) )
                {
                    target[ output++ ] = source.at( left++ );
                }
                else
                    target[ output++ ] = source.at( right++ );
            }
        }

        sourceIsItems = !sourceIsItems;
        if( pairWidth >= items.size() )
            break;
        width = pairWidth;
    }

    if( !sourceIsItems )
        items.swap( scratch );
    return !stopToken.stop_requested();
}

QHash< QString, int > recentRanks( const QStringList& recentRelativePaths )
{
    QHash< QString, int > ranks;
    for( int index = 0; index < recentRelativePaths.size(); ++index )
    {
        const QString key = normalizeQuickOpenPath( recentRelativePaths.at( index ) ).toCaseFolded();
        if( !key.isEmpty() && !ranks.contains( key ) )
            ranks.insert( key, index );
    }
    return ranks;
}

QStringList pathParts( const QString& normalizedPath )
{
    return normalizedPath.split( QLatin1Char( '/' ), Qt::SkipEmptyParts );
}

RankedMatch makeBaseMatch( const QString& path, const qsizetype inputOrder,
                           const QHash< QString, int >& recents )
{
    RankedMatch ranked;
    ranked.tier = static_cast<int>( MatchTier::Path );
    ranked.inputOrder = inputOrder;
    ranked.match.relativePath = path;

    const qsizetype separator = path.lastIndexOf( QLatin1Char( '/' ) );
    if( separator < 0 )
        ranked.match.fileName = path;
    else
    {
        ranked.match.directoryPath = path.left( separator );
        ranked.match.fileName = path.mid( separator + 1 );
    }

    ranked.recentRank = recents.value( path.toCaseFolded(), kNotRecent );
    return ranked;
}

qsizetype pathCount( const QStringList& paths )
{
    return paths.size();
}

qsizetype pathCount( const QuickOpenPathChunks& chunks )
{
    qsizetype count = 0;
    for( const QuickOpenPathChunk& chunk : chunks )
    {
        if( chunk != nullptr )
            count += chunk->size();
    }
    return count;
}

template< typename Visitor >
bool visitRelativePaths( const QStringList& paths, const qsizetype inputOrderOffset,
                         const std::stop_token stopToken, Visitor&& visitor )
{
    for( qsizetype index = 0; index < paths.size(); ++index )
    {
        if( stopToken.stop_requested() )
            return false;
        visitor( paths.at( index ), inputOrderOffset + index );
    }
    return !stopToken.stop_requested();
}

template< typename Visitor >
bool visitRelativePaths( const QuickOpenPathChunks& chunks,
                         const qsizetype inputOrderOffset,
                         const std::stop_token stopToken, Visitor&& visitor )
{
    qsizetype inputOrder = inputOrderOffset;
    for( const QuickOpenPathChunk& chunk : chunks )
    {
        if( stopToken.stop_requested() )
            return false;
        if( chunk == nullptr )
            continue;

        for( const QString& path : *chunk )
        {
            if( stopToken.stop_requested() )
                return false;
            visitor( path, inputOrder++ );
        }
    }
    return !stopToken.stop_requested();
}

template< typename PathCollection >
QuickOpenRankedMatches rankQuickOpenFilesDetailedImpl(
    const PathCollection& relativePaths, const QString& query,
    const QStringList& recentRelativePaths, const qsizetype inputOrderOffset,
    const std::stop_token stopToken )
{
    if( stopToken.stop_requested() || query.size() > kQuickOpenMaximumQueryLength )
        return {};

    const QHash< QString, int > recents = recentRanks( recentRelativePaths );
    QVector< RankedMatch > ranked;
    ranked.reserve( pathCount( relativePaths ) );

    QString separatedQuery = query;
    separatedQuery.replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) );
    const bool hasPathSeparator = separatedQuery.contains( QLatin1Char( '/' ) );
    const bool trailingSeparator = separatedQuery.endsWith( QLatin1Char( '/' ) );
    const QString normalizedQuery = normalizeQuickOpenPath( separatedQuery );

    if( normalizedQuery.isEmpty() && !hasPathSeparator )
    {
        const bool completed = visitRelativePaths(
            relativePaths, inputOrderOffset, stopToken,
            [ &ranked, &recents ]( const QString& sourcePath, const qsizetype inputOrder ) {
                const QString path = normalizeQuickOpenPath( sourcePath );
                if( !path.isEmpty() )
                    ranked.push_back( makeBaseMatch( path, inputOrder, recents ) );
            } );
        if( !completed )
            return {};

        const auto less = []( const RankedMatch& left, const RankedMatch& right ) {
            if( left.recentRank != right.recentRank )
                return left.recentRank < right.recentRank;
            return alphabeticalCompare( left, right ) < 0;
        };
        if( !cancellableSort( ranked, less, stopToken ) )
            return {};
    }
    else
    {
        QStringList queryParts = pathParts( normalizedQuery );
        QString filePattern;
        QStringList directoryPatterns;
        if( hasPathSeparator )
        {
            directoryPatterns = queryParts;
            if( !trailingSeparator && !directoryPatterns.isEmpty() )
                filePattern = directoryPatterns.takeLast();
        }
        else
            filePattern = normalizedQuery;

        const bool completed = visitRelativePaths(
            relativePaths, inputOrderOffset, stopToken,
            [ &ranked, &recents, &directoryPatterns, &filePattern, hasPathSeparator,
              trailingSeparator ]( const QString& sourcePath, const qsizetype inputOrder ) {
                const QString path = normalizeQuickOpenPath( sourcePath );
                if( path.isEmpty() )
                    return;

                RankedMatch candidate = makeBaseMatch( path, inputOrder, recents );
                QStringList candidateParts = pathParts( path );
                if( candidateParts.isEmpty() )
                    return;
                candidateParts.removeLast();

                if( hasPathSeparator )
                {
                    const PathHit directoryHit =
                        matchDirectorySegments( directoryPatterns, candidateParts );
                    if( !directoryHit.matched )
                        return;

                    candidate.match.pathMatchedPositions = directoryHit.positions;
                    candidate.score = directoryHit.score - candidateParts.size() * 12;

                    if( trailingSeparator || filePattern.isEmpty() )
                        candidate.tier = static_cast<int>( MatchTier::Path );
                    else
                    {
                        const FileNameHit fileHit =
                            matchFileName( filePattern, candidate.match.fileName );
                        if( !fileHit.matched )
                            return;
                        candidate.tier = static_cast<int>( fileHit.tier );
                        candidate.score += fileHit.score;
                        candidate.match.fileNameMatchedPositions = fileHit.positions;
                    }
                }
                else
                {
                    const FileNameHit fileHit =
                        matchFileName( filePattern, candidate.match.fileName );
                    if( fileHit.matched )
                    {
                        candidate.tier = static_cast<int>( fileHit.tier );
                        candidate.score = fileHit.score - candidateParts.size() * 12;
                        candidate.match.fileNameMatchedPositions = fileHit.positions;
                    }
                    else
                    {
                        const FuzzyHit pathHit = fuzzyMatch(
                            filePattern, candidate.match.directoryPath );
                        if( !pathHit.matched )
                            return;
                        candidate.tier = static_cast<int>( MatchTier::Path );
                        candidate.score = pathHit.score - candidateParts.size() * 12;
                        candidate.match.pathMatchedPositions = pathHit.positions;
                    }
                }

                ranked.push_back( std::move( candidate ) );
            } );
        if( !completed )
            return {};

        const auto less = []( const RankedMatch& left, const RankedMatch& right ) {
            if( left.tier != right.tier )
                return left.tier < right.tier;
            if( left.score != right.score )
                return left.score > right.score;
            if( left.recentRank != right.recentRank )
                return left.recentRank < right.recentRank;
            return alphabeticalCompare( left, right ) < 0;
        };
        if( !cancellableSort( ranked, less, stopToken ) )
            return {};
    }

    if( stopToken.stop_requested() )
        return {};
    return ranked;
}

bool usesEmptyQueryOrdering( const QString& query )
{
    QString separatedQuery = query;
    separatedQuery.replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) );
    return !separatedQuery.contains( QLatin1Char( '/' ) )
           && normalizeQuickOpenPath( separatedQuery ).isEmpty();
}

bool rankedMatchLess( const RankedMatch& left, const RankedMatch& right,
                      const bool emptyQueryOrdering )
{
    if( !emptyQueryOrdering )
    {
        if( left.tier != right.tier )
            return left.tier < right.tier;
        if( left.score != right.score )
            return left.score > right.score;
    }
    if( left.recentRank != right.recentRank )
        return left.recentRank < right.recentRank;
    return alphabeticalCompare( left, right ) < 0;
}

QVector< QuickOpenMatch > takeMatches( QuickOpenRankedMatches ranked,
                                      const std::stop_token stopToken )
{
    QVector< QuickOpenMatch > results;
    results.reserve( ranked.size() );
    for( RankedMatch& item : ranked )
    {
        if( stopToken.stop_requested() )
            return {};
        results.push_back( std::move( item.match ) );
    }
    return results;
}

}   // namespace

QString normalizeQuickOpenPath( const QString& path )
{
    QString separated = path;
    separated.replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) );

    QStringList normalizedParts;
    const QStringList parts = separated.split( QLatin1Char( '/' ), Qt::SkipEmptyParts );
    normalizedParts.reserve( parts.size() );
    for( const QString& part : parts )
    {
        if( part == QLatin1String( "." ) )
            continue;
        if( part == QLatin1String( ".." ) && !normalizedParts.isEmpty()
            && normalizedParts.constLast() != QLatin1String( ".." ) )
        {
            normalizedParts.removeLast();
            continue;
        }
        normalizedParts.push_back( part );
    }
    return normalizedParts.join( QLatin1Char( '/' ) );
}

QVector< QuickOpenMatch > rankQuickOpenFiles( const QStringList& relativePaths,
                                              const QString& query,
                                              const QStringList& recentRelativePaths,
                                              const std::stop_token stopToken )
{
    return takeMatches( rankQuickOpenFilesDetailedImpl(
                            relativePaths, query, recentRelativePaths, 0, stopToken ),
                        stopToken );
}

QVector< QuickOpenMatch > rankQuickOpenFileChunks(
    const QuickOpenPathChunks& relativePathChunks, const QString& query,
    const QStringList& recentRelativePaths, const std::stop_token stopToken )
{
    return takeMatches( rankQuickOpenFilesDetailedImpl(
                            relativePathChunks, query, recentRelativePaths, 0, stopToken ),
                        stopToken );
}

QuickOpenRankedMatches rankQuickOpenFileChunksDetailed(
    const QuickOpenPathChunks& relativePathChunks, const QString& query,
    const QStringList& recentRelativePaths, const qsizetype inputOrderOffset,
    const std::stop_token stopToken )
{
    return rankQuickOpenFilesDetailedImpl( relativePathChunks, query,
                                           recentRelativePaths, inputOrderOffset,
                                           stopToken );
}

QuickOpenRankedMatches mergeQuickOpenRankedMatches(
    const QuickOpenRankedMatches& existing, QuickOpenRankedMatches added,
    const QString& query, const std::stop_token stopToken )
{
    if( stopToken.stop_requested() )
        return {};

    const bool emptyQueryOrdering = usesEmptyQueryOrdering( query );
    QuickOpenRankedMatches merged;
    merged.reserve( existing.size() + added.size() );
    qsizetype left = 0;
    qsizetype right = 0;
    constexpr qsizetype kCancelCheckStride = 1'024;
    while( left < existing.size() || right < added.size() )
    {
        if( merged.size() % kCancelCheckStride == 0 && stopToken.stop_requested() )
            return {};

        if( right >= added.size()
            || ( left < existing.size()
                 && rankedMatchLess( existing.at( left ), added.at( right ),
                                     emptyQueryOrdering ) ) )
        {
            merged.push_back( existing.at( left++ ) );
        }
        else
            merged.push_back( std::move( added[ right++ ] ) );
    }
    return stopToken.stop_requested() ? QuickOpenRankedMatches{} : std::move( merged );
}

}   // namespace mrst
