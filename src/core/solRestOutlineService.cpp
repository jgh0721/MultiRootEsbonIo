#include "stdafx.h"
#include "solRestOutlineService.hpp"

#include "core/solFileKinds.hpp"
#include "utils/solBackgroundWork.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace mrst {
namespace {

/// docutils 가 섹션 밑줄로 인정하는 문자.
const QString& adornmentChars()
{
    static const QString characters = QStringLiteral( R"(=-`:'"~^_*+#<>)" );
    return characters;
}

/// LSP SymbolKind::Number. 파이썬 원본과 맞춘다 (섹션에 딱 맞는 kind 가 없다).
constexpr int kSectionKind = 12;

/// 전부 같은 장식 문자로 이루어진 두 글자 이상의 줄인가.
QChar adornmentChar( const QStringView line )
{
    if( line.length() < 2 )
        return {};

    const QChar first = line.at( 0 );
    if( !adornmentChars().contains( first ) )
        return {};

    for( const QChar character : line )
    {
        if( character != first )
            return {};
    }
    return first;
}

struct Heading
{
    QString                             name;
    int                                 line = 1;   ///< 1-based, 제목 줄
    QString                             style;      ///< "over:=" / "under:-"
};

QVector< Heading > sectionHeadings( const QString& text )
{
    const QStringList lines = text.split( QLatin1Char( '\n' ) );
    QVector< Heading > headings;

    for( qsizetype index = 0; index < lines.size(); )
    {
        const QString stripped = lines.at( index ).trimmed();

        // 윗줄+아랫줄 형식:
        //   =====
        //   제목
        //   =====
        if( const QChar over = adornmentChar( stripped ); !over.isNull() && index + 2 < lines.size() )
        {
            const QString title = lines.at( index + 1 ).trimmed();
            const QString under = lines.at( index + 2 ).trimmed();
            if( !title.isEmpty() && adornmentChar( under ) == over && under.length() >= title.length() )
            {
                headings.push_back( { title, static_cast< int >( index + 2 ),
                                     QStringLiteral( "over:%1" ).arg( over ) } );
                index += 3;
                continue;
            }
        }

        // 아랫줄만 있는 형식:
        //   제목
        //   =====
        if( !stripped.isEmpty() && adornmentChar( stripped ).isNull() && index + 1 < lines.size() )
        {
            const QString under = lines.at( index + 1 ).trimmed();
            const QChar character = adornmentChar( under );
            if( !character.isNull() && under.length() >= stripped.length() )
            {
                headings.push_back( { stripped, static_cast< int >( index + 1 ),
                                     QStringLiteral( "under:%1" ).arg( character ) } );
                index += 2;
                continue;
            }
        }

        ++index;
    }
    return headings;
}

}  // namespace

QVector< OutlineSymbol > parseRstOutline( const QString& text, const QString& path )
{
    const QVector< Heading > headings = sectionHeadings( text );
    if( headings.isEmpty() )
        return {};

    QVector< OutlineSymbol > roots;
    // 부모를 가리키는 포인터를 담으면 벡터가 재할당될 때 전부 무효가 된다.
    // 루트에서부터의 자식 인덱스 경로로 들고 있는다.
    struct StackEntry
    {
        int                             level;
        QVector< int >                  indexPath;
    };
    QVector< StackEntry > stack;
    QHash< QString, int > styleLevels;

    auto resolve = [ &roots ]( const QVector< int >& indexPath ) -> QVector< OutlineSymbol >& {
        QVector< OutlineSymbol >* siblings = &roots;
        for( const int index : indexPath )
            siblings = &( ( *siblings )[ index ].children );
        return *siblings;
    };

    for( const Heading& heading : headings )
    {
        // 계층은 밑줄 문자가 **처음 나온 순서** 로 정해진다. docutils 규칙이다.
        const int level = styleLevels.value( heading.style, styleLevels.size() + 1 );
        styleLevels.insert( heading.style, level );

        while( !stack.isEmpty() && stack.last().level >= level )
            stack.removeLast();

        OutlineSymbol symbol;
        symbol.name = heading.name;
        symbol.detail = QStringLiteral( "reST" );
        symbol.kind = kSectionKind;
        symbol.line = heading.line;
        symbol.path = path;

        QVector< int > parentPath = stack.isEmpty() ? QVector< int >{} : stack.last().indexPath;
        QVector< OutlineSymbol >& siblings = resolve( parentPath );
        siblings.push_back( symbol );

        parentPath.push_back( static_cast< int >( siblings.size() ) - 1 );
        stack.push_back( { level, parentPath } );
    }

    return roots;
}

QVector< OutlineSymbol > toOutlineSymbols( const QList< LspDocumentSymbol >& symbols,
                                           const QString& path )
{
    QVector< OutlineSymbol > result;
    result.reserve( symbols.size() );
    for( const LspDocumentSymbol& symbol : symbols )
    {
        OutlineSymbol converted;
        converted.name = symbol.name;
        converted.detail = symbol.detail;
        converted.kind = symbol.kind;
        converted.line = qMax( 1, symbol.line );
        converted.path = path;
        converted.children = toOutlineSymbols( symbol.children, path );
        result.push_back( converted );
    }
    return result;
}

QStringList collectProjectDocuments( const QString& sourceRoot, const QString& rootDoc,
                                     const int limit, int* totalFound )
{
    const QDir root( QFileInfo( sourceRoot ).absoluteFilePath() );
    if( !root.exists() )
    {
        if( totalFound != nullptr )
            *totalFound = 0;
        return {};
    }

    static const QStringList suffixes{ QStringLiteral( "rst" ), QStringLiteral( "txt" ),
                                      QStringLiteral( "md" ) };

    QStringList documents;
    QDirIterator iterator( root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories );
    while( iterator.hasNext() )
    {
        // 앱이 내려가는 중이면 그만둔다. 여기까지 모은 것은 어차피 버려진다.
        // 끝까지 도는 동안 프로세스가 살아 있는 것이 실제 문제였다.
        if( isShuttingDown() )
            return {};

        const QString absolute = iterator.next();
        const QFileInfo info( absolute );
        if( !suffixes.contains( info.suffix().toCaseFolded() ) )
            continue;

        const QString relative = root.relativeFilePath( absolute );
        const QStringList parts = relative.split( QLatin1Char( '/' ), Qt::SkipEmptyParts );
        bool excluded = false;
        for( qsizetype index = 0; index + 1 < parts.size(); ++index )
            excluded = excluded || filekinds::isExcludedDirectoryName( parts.at( index ) );
        if( excluded )
            continue;

        documents << QDir::cleanPath( absolute );
    }

    // root_doc 이 맨 앞. 나머지는 상대경로순.
    const QString rootStem = QFileInfo( rootDoc ).path() == QStringLiteral( "." )
                                 ? QFileInfo( rootDoc ).completeBaseName().toCaseFolded()
                                 : ( QFileInfo( rootDoc ).path() + QLatin1Char( '/' )
                                    + QFileInfo( rootDoc ).completeBaseName() )
                                       .toCaseFolded();

    auto sortKey = [ &root ]( const QString& path ) {
        const QString relative = root.relativeFilePath( path );
        const qsizetype dot = relative.lastIndexOf( QLatin1Char( '.' ) );
        return QPair< QString, QString >{ dot > 0 ? relative.left( dot ).toCaseFolded()
                                                  : relative.toCaseFolded(),
                                         relative.toCaseFolded() };
    };

    std::sort( documents.begin(), documents.end(),
              [ & ]( const QString& left, const QString& right ) {
                  const auto leftKey = sortKey( left );
                  const auto rightKey = sortKey( right );
                  const bool leftIsRoot = leftKey.first == rootStem;
                  const bool rightIsRoot = rightKey.first == rootStem;
                  if( leftIsRoot != rightIsRoot )
                      return leftIsRoot;
                  return leftKey.second < rightKey.second;
              } );

    if( totalFound != nullptr )
        *totalFound = static_cast< int >( documents.size() );

    if( limit > 0 && documents.size() > limit )
        documents = documents.mid( 0, limit );
    return documents;
}

QVector< OutlineDocumentEntry > buildProjectOutline( const QString& sourceRoot,
                                                     const QStringList& documents )
{
    const QDir root( QFileInfo( sourceRoot ).absoluteFilePath() );

    QVector< OutlineDocumentEntry > entries;
    entries.reserve( documents.size() );
    for( const QString& path : documents )
    {
        // 문서마다 파일을 열고 전부 읽는다(상한 500). 종료 중이면 멈춘다.
        if( isShuttingDown() )
            return {};

        OutlineDocumentEntry entry;
        entry.path = path;
        entry.label = root.relativeFilePath( path );
        if( entry.label.startsWith( QStringLiteral( ".." ) ) )
            entry.label = QFileInfo( path ).fileName();

        QFile file( path );
        if( file.open( QIODevice::ReadOnly | QIODevice::Text ) )
        {
            // 개요만 뽑으므로 인코딩 추정까지 하지 않는다. UTF-8 이 아니면
            // 제목이 깨져 보일 수는 있어도 줄 번호는 맞다.
            entry.symbols = parseRstOutline( QString::fromUtf8( file.readAll() ), path );
        }
        entries.push_back( entry );
    }
    return entries;
}

}  // namespace mrst
