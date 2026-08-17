#include "stdafx.h"
#include "solWorkspaceSearch.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace mrst {
namespace {

const QStringList& defaultSuffixes()
{
    static const QStringList suffixes{
        QStringLiteral( "rst" ),  QStringLiteral( "txt" ),  QStringLiteral( "md" ),
        QStringLiteral( "py" ),   QStringLiteral( "toml" ), QStringLiteral( "json" ),
        QStringLiteral( "yaml" ), QStringLiteral( "yml" ),  QStringLiteral( "cfg" ),
        QStringLiteral( "ini" ),
    };
    return suffixes;
}

const QSet< QString >& excludedDirectories()
{
    static const QSet< QString > names{
        QStringLiteral( ".git" ),   QStringLiteral( ".hg" ),   QStringLiteral( ".svn" ),
        QStringLiteral( ".idea" ),  QStringLiteral( ".vs" ),   QStringLiteral( "__pycache__" ),
        QStringLiteral( "_build" ), QStringLiteral( "build" ), QStringLiteral( ".multiroot" ),
        QStringLiteral( ".venv" ),  QStringLiteral( "venv" ),  QStringLiteral( "env" ),
        QStringLiteral( "node_modules" ), QStringLiteral( ".tox" ),
    };
    return names;
}

/// 검색/치환에 쓸 정규식 하나로 정규화한다.
/// 리터럴 검색도 escape 해서 같은 경로를 타면 분기가 줄어든다.
QRegularExpression buildPattern( const QString& query, const SearchOptions& options )
{
    QString pattern = options.regex ? query : QRegularExpression::escape( query );
    if( options.wholeWords )
        pattern = QStringLiteral( "\\b(?:%1)\\b" ).arg( pattern );

    QRegularExpression::PatternOptions flags = QRegularExpression::NoPatternOption;
    if( !options.caseSensitive )
        flags |= QRegularExpression::CaseInsensitiveOption;

    return QRegularExpression( pattern, flags );
}

QString readTextFile( const QString& path, bool* ok = nullptr )
{
    QFile file( path );
    if( !file.open( QIODevice::ReadOnly ) )
    {
        if( ok != nullptr )
            *ok = false;
        return {};
    }
    if( ok != nullptr )
        *ok = true;

    // 개행은 있는 그대로 둔다. 치환 후 파일을 다시 쓸 때 원래 줄 끝을 지키려면
    // 여기서 변환해 버리면 안 된다.
    return QString::fromUtf8( file.readAll() );
}

}  // namespace

QStringList collectSearchableFiles( const QString& root, const QStringList& suffixes )
{
    const QDir rootDir( QFileInfo( root ).absoluteFilePath() );
    if( !rootDir.exists() )
        return {};

    QStringList wanted;
    for( const QString& suffix : ( suffixes.isEmpty() ? defaultSuffixes() : suffixes ) )
        wanted << suffix.toCaseFolded();

    QStringList files;
    QDirIterator iterator( rootDir.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories );
    while( iterator.hasNext() )
    {
        const QString absolute = iterator.next();
        if( !wanted.contains( QFileInfo( absolute ).suffix().toCaseFolded() ) )
            continue;

        const QStringList parts =
            rootDir.relativeFilePath( absolute ).split( QLatin1Char( '/' ), Qt::SkipEmptyParts );
        bool excluded = false;
        for( qsizetype index = 0; index + 1 < parts.size(); ++index )
            excluded = excluded || excludedDirectories().contains( parts.at( index ).toCaseFolded() );
        if( excluded )
            continue;

        files << QDir::cleanPath( absolute );
    }

    files.sort( Qt::CaseInsensitive );
    return files;
}

QVector< SearchMatch > findInFiles( const QString& root, const QString& query,
                                    const SearchOptions& options )
{
    QVector< SearchMatch > matches;
    if( query.isEmpty() )
        return matches;

    const QRegularExpression pattern = buildPattern( query, options );
    if( !pattern.isValid() )
        return matches;

    for( const QString& path : collectSearchableFiles( root, options.suffixes ) )
    {
        bool ok = false;
        const QString content = readTextFile( path, &ok );
        if( !ok )
            continue;

        const QStringList lines = content.split( QLatin1Char( '\n' ) );
        for( qsizetype index = 0; index < lines.size(); ++index )
        {
            const QString line = lines.at( index );
            QRegularExpressionMatchIterator iterator = pattern.globalMatch( line );
            while( iterator.hasNext() )
            {
                const QRegularExpressionMatch match = iterator.next();
                matches.push_back( { path, static_cast< int >( index ) + 1,
                                    static_cast< int >( match.capturedStart() ) + 1,
                                    line.trimmed() } );
                if( matches.size() >= options.maxMatches )
                    return matches;
            }
        }
    }
    return matches;
}

QVector< ReplacePreview > previewReplaceInFiles( const QString& root, const QString& query,
                                                 const QString& replacement,
                                                 const SearchOptions& options, const int contextLines )
{
    QVector< ReplacePreview > previews;
    if( query.isEmpty() )
        return previews;

    const QRegularExpression pattern = buildPattern( query, options );
    if( !pattern.isValid() )
        return previews;

    for( const QString& path : collectSearchableFiles( root, options.suffixes ) )
    {
        bool ok = false;
        const QString before = readTextFile( path, &ok );
        if( !ok )
            continue;

        int count = 0;
        QRegularExpressionMatchIterator counter = pattern.globalMatch( before );
        while( counter.hasNext() )
        {
            counter.next();
            ++count;
        }
        if( count == 0 )
            continue;

        QString after = before;
        after.replace( pattern, replacement );

        previews.push_back( { path, count,
                             unifiedDiff( before, after, QDir( root ).relativeFilePath( path ),
                                         contextLines ) } );
    }
    return previews;
}

QStringList applyReplaceInFiles( const QStringList& paths, const QString& query,
                                 const QString& replacement, const SearchOptions& options )
{
    QStringList changed;
    if( query.isEmpty() )
        return changed;

    const QRegularExpression pattern = buildPattern( query, options );
    if( !pattern.isValid() )
        return changed;

    for( const QString& path : paths )
    {
        bool ok = false;
        const QString before = readTextFile( path, &ok );
        if( !ok )
            continue;

        QString after = before;
        after.replace( pattern, replacement );
        if( after == before )
            continue;

        QFile file( path );
        if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
            continue;
        if( file.write( after.toUtf8() ) < 0 )
            continue;

        changed << path;
    }
    return changed;
}

// ── unified diff ──────────────────────────────────────────

QStringList unifiedDiffLines( const QStringList& before, const QStringList& after,
                              const int contextLines )
{
    // LCS 길이 표. 문서 파일 규모(수천 줄)에서 O(n*m) 은 충분히 빠르다.
    const qsizetype n = before.size();
    const qsizetype m = after.size();
    QVector< QVector< int > > lcs( n + 1, QVector< int >( m + 1, 0 ) );
    for( qsizetype i = n; i-- > 0; )
    {
        for( qsizetype j = m; j-- > 0; )
        {
            lcs[ i ][ j ] = before.at( i ) == after.at( j )
                                ? lcs[ i + 1 ][ j + 1 ] + 1
                                : qMax( lcs[ i + 1 ][ j ], lcs[ i ][ j + 1 ] );
        }
    }

    // 편집 스크립트를 만든다. ' ' 유지 / '-' 삭제 / '+' 추가.
    struct Op
    {
        QChar                           kind;
        QString                         text;
        qsizetype                       beforeLine;   ///< 0-based, 추가면 -1
        qsizetype                       afterLine;    ///< 0-based, 삭제면 -1
    };
    QVector< Op > script;
    qsizetype i = 0;
    qsizetype j = 0;
    while( i < n && j < m )
    {
        if( before.at( i ) == after.at( j ) )
        {
            script.push_back( { QLatin1Char( ' ' ), before.at( i ), i, j } );
            ++i;
            ++j;
        }
        else if( lcs[ i + 1 ][ j ] >= lcs[ i ][ j + 1 ] )
        {
            script.push_back( { QLatin1Char( '-' ), before.at( i ), i, -1 } );
            ++i;
        }
        else
        {
            script.push_back( { QLatin1Char( '+' ), after.at( j ), -1, j } );
            ++j;
        }
    }
    for( ; i < n; ++i )
        script.push_back( { QLatin1Char( '-' ), before.at( i ), i, -1 } );
    for( ; j < m; ++j )
        script.push_back( { QLatin1Char( '+' ), after.at( j ), -1, j } );

    // 변경된 위치 주변 contextLines 만 남긴다.
    QVector< bool > keep( script.size(), false );
    bool anyChange = false;
    for( qsizetype index = 0; index < script.size(); ++index )
    {
        if( script.at( index ).kind == QLatin1Char( ' ' ) )
            continue;
        anyChange = true;
        const qsizetype from = qMax( qsizetype( 0 ), index - contextLines );
        const qsizetype to = qMin( script.size() - 1, index + contextLines );
        for( qsizetype k = from; k <= to; ++k )
            keep[ k ] = true;
    }
    if( !anyChange )
        return {};

    QStringList output;
    qsizetype index = 0;
    while( index < script.size() )
    {
        if( !keep.at( index ) )
        {
            ++index;
            continue;
        }

        const qsizetype start = index;
        while( index < script.size() && keep.at( index ) )
            ++index;
        const qsizetype end = index;   // exclusive

        // hunk 머리말의 시작 줄은 이 구간에서 처음 등장하는 실제 줄 번호.
        qsizetype beforeStart = -1;
        qsizetype afterStart = -1;
        int beforeCount = 0;
        int afterCount = 0;
        for( qsizetype k = start; k < end; ++k )
        {
            const Op& op = script.at( k );
            if( op.kind != QLatin1Char( '+' ) )
            {
                if( beforeStart < 0 )
                    beforeStart = op.beforeLine;
                ++beforeCount;
            }
            if( op.kind != QLatin1Char( '-' ) )
            {
                if( afterStart < 0 )
                    afterStart = op.afterLine;
                ++afterCount;
            }
        }

        output << QStringLiteral( "@@ -%1,%2 +%3,%4 @@" )
                     .arg( beforeStart < 0 ? 0 : beforeStart + 1 )
                     .arg( beforeCount )
                     .arg( afterStart < 0 ? 0 : afterStart + 1 )
                     .arg( afterCount );

        for( qsizetype k = start; k < end; ++k )
            output << script.at( k ).kind + script.at( k ).text;
    }
    return output;
}

QString unifiedDiff( const QString& before, const QString& after, const QString& label,
                     const int contextLines )
{
    const QStringList hunks = unifiedDiffLines( before.split( QLatin1Char( '\n' ) ),
                                               after.split( QLatin1Char( '\n' ) ), contextLines );
    if( hunks.isEmpty() )
        return {};

    QStringList output;
    output << QStringLiteral( "--- a/%1" ).arg( label );
    output << QStringLiteral( "+++ b/%1" ).arg( label );
    output << hunks;
    return output.join( QLatin1Char( '\n' ) );
}

}  // namespace mrst
