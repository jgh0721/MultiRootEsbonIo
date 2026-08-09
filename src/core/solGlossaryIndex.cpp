#include "stdafx.h"
#include "core/solGlossaryIndex.hpp"

#include "core/solRestOutlineService.hpp"

#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>

namespace mrst {

namespace {

/// 용어집을 뽑을 때 훑을 문서 수의 상한. 프로젝트 개요와 같은 규모로 잡는다.
constexpr int kMaxGlossaryDocuments = 2000;

/// 줄 앞 공백의 개수. 탭은 8칸으로 센다 (docutils 규칙).
int indentWidth( const QString& line )
{
    int width = 0;
    for( const QChar ch : line )
    {
        if( ch == QLatin1Char( ' ' ) )
            ++width;
        else if( ch == QLatin1Char( '\t' ) )
            width += 8 - ( width % 8 );
        else
            break;
    }
    return width;
}

bool isBlank( const QString& line )
{
    return line.trimmed().isEmpty();
}

/// `용어 : 정렬키` 에서 표시용 용어만 남긴다.
/// 이스케이프한 `\:` 는 콜론 그대로 둔다.
QString stripSortKey( QString term )
{
    for( qsizetype index = 0; index + 1 < term.size(); ++index )
    {
        if( term.at( index ) != QLatin1Char( ' ' ) || term.at( index + 1 ) != QLatin1Char( ':' ) )
            continue;
        if( index > 0 && term.at( index - 1 ) == QLatin1Char( '\\' ) )
            continue;
        term = term.left( index );
        break;
    }
    return term.replace( QStringLiteral( "\\:" ), QStringLiteral( ":" ) ).trimmed();
}

}  // namespace

QVector< GlossaryEntry > parseGlossary( const QString& text, const QString& path )
{
    static const QRegularExpression glossaryRe(
        QStringLiteral( R"(^(\s*)\.\.\s+glossary::\s*$)" ) );

    const QStringList lines = text.split( QRegularExpression( QStringLiteral( "\r\n|\n|\r" ) ) );

    QVector< GlossaryEntry > entries;
    for( qsizetype index = 0; index < lines.size(); ++index )
    {
        const QRegularExpressionMatch match = glossaryRe.match( lines.at( index ) );
        if( !match.hasMatch() )
            continue;

        const int directiveIndent = indentWidth( lines.at( index ) );

        // directive 본문: 이후로 나오는 "더 깊게 들여쓴" 줄들. 빈 줄은 본문에 속한다.
        qsizetype bodyEnd = index + 1;
        int       bodyIndent = -1;
        for( qsizetype scan = index + 1; scan < lines.size(); ++scan )
        {
            const QString& line = lines.at( scan );
            if( isBlank( line ) )
                continue;
            const int width = indentWidth( line );
            if( width <= directiveIndent )
                break;
            if( bodyIndent < 0 || width < bodyIndent )
                bodyIndent = width;
            bodyEnd = scan + 1;
        }
        if( bodyIndent < 0 )
        {
            index = bodyEnd - 1;
            continue;
        }

        // 본문 안의 정의 목록: bodyIndent 인 줄이 용어, 더 깊은 줄이 정의.
        // `:sorted:` 같은 directive 옵션은 용어가 아니다.
        qsizetype cursor = index + 1;
        while( cursor < bodyEnd )
        {
            if( isBlank( lines.at( cursor ) ) || indentWidth( lines.at( cursor ) ) != bodyIndent )
            {
                ++cursor;
                continue;
            }

            const QString head = lines.at( cursor ).trimmed();
            if( head.startsWith( QLatin1Char( ':' ) ) )
            {
                ++cursor;
                continue;
            }

            // 연속된 용어 줄 = 동의어
            QStringList terms;
            QVector< int > termLines;
            while( cursor < bodyEnd && !isBlank( lines.at( cursor ) )
                   && indentWidth( lines.at( cursor ) ) == bodyIndent )
            {
                const QString term = stripSortKey( lines.at( cursor ).trimmed() );
                if( !term.isEmpty() )
                {
                    terms << term;
                    termLines << static_cast< int >( cursor ) + 1;
                }
                ++cursor;
            }
            if( terms.isEmpty() )
                continue;

            // 정의: 다음에 나오는 더 깊게 들여쓴 블록
            QStringList definitionLines;
            int         definitionIndent = -1;
            while( cursor < bodyEnd )
            {
                const QString& line = lines.at( cursor );
                if( isBlank( line ) )
                {
                    if( !definitionLines.isEmpty() )
                        definitionLines << QString();
                    ++cursor;
                    continue;
                }
                const int width = indentWidth( line );
                if( width <= bodyIndent )
                    break;
                if( definitionIndent < 0 )
                    definitionIndent = width;
                definitionLines << line.mid( qMin< qsizetype >( definitionIndent, line.size() ) );
                ++cursor;
            }

            while( !definitionLines.isEmpty() && definitionLines.constLast().isEmpty() )
                definitionLines.removeLast();

            const QString definition = definitionLines.join( QLatin1Char( '\n' ) );
            for( qsizetype termIndex = 0; termIndex < terms.size(); ++termIndex )
            {
                GlossaryEntry entry;
                entry.term = terms.at( termIndex );
                entry.definition = definition;
                entry.path = path;
                entry.line = termLines.at( termIndex );
                entries.push_back( entry );
            }
        }

        index = bodyEnd - 1;
    }

    return entries;
}

GlossaryIndex::GlossaryIndex( QObject* parent )
    : QObject( parent )
{
}

void GlossaryIndex::setActiveProjectId( const QString& projectId )
{
    activeProjectId_ = projectId;
}

void GlossaryIndex::refresh( const QString& projectId, const QString& sourceRoot,
                             const QString& rootDoc, const bool force )
{
    if( projectId.isEmpty() || sourceRoot.isEmpty() )
        return;
    if( !force && indexedProjectId_ == projectId )
        return;

    indexedProjectId_ = projectId;
    const quint64 generation = ++generation_;
    QPointer< GlossaryIndex > guard( this );

    // 문서 수백 개를 읽는 일이라 GUI 스레드에서 하면 눈에 띄게 멈춘다.
    QThreadPool::globalInstance()->start( [guard, sourceRoot, rootDoc, projectId, generation] {
        const QStringList paths = collectProjectDocuments( sourceRoot, rootDoc, kMaxGlossaryDocuments );

        QVector< GlossaryEntry > entries;
        for( const QString& path : paths )
        {
            QFile file( path );
            if( !file.open( QIODevice::ReadOnly | QIODevice::Text ) )
                continue;
            const QString text = QString::fromUtf8( file.readAll() );
            entries += parseGlossary( text, path );
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, entries = std::move( entries ), projectId, generation]() mutable {
                if( guard )
                    guard->apply( projectId, std::move( entries ), generation );
            },
            Qt::QueuedConnection );
    } );
}

void GlossaryIndex::apply( const QString& projectId, QVector< GlossaryEntry > entries,
                           const quint64 generation )
{
    // 프로젝트가 바뀐 뒤 도착한 결과는 버린다.
    if( generation != generation_ )
        return;

    entries_ = std::move( entries );
    byLowerTerm_.clear();
    byLowerTerm_.reserve( static_cast< int >( entries_.size() ) );
    for( qsizetype index = 0; index < entries_.size(); ++index )
    {
        // 같은 용어가 여러 문서에 있으면 먼저 만난 것을 쓴다.
        byLowerTerm_.insert( entries_.at( index ).term.toCaseFolded(), static_cast< int >( index ) );
    }

    emit ready( projectId, static_cast< int >( entries_.size() ) );
}

const GlossaryEntry* GlossaryIndex::lookup( const QString& term ) const
{
    const auto it = byLowerTerm_.constFind( term.trimmed().toCaseFolded() );
    if( it == byLowerTerm_.constEnd() )
        return nullptr;
    return &entries_.at( *it );
}

QVector< GlossaryEntry > GlossaryIndex::match( const QString& prefix, const int limit ) const
{
    const QString folded = prefix.trimmed().toCaseFolded();

    QVector< GlossaryEntry > matched;
    for( const GlossaryEntry& entry : entries_ )
    {
        if( !folded.isEmpty() && !entry.term.toCaseFolded().startsWith( folded ) )
            continue;
        matched.push_back( entry );
        if( matched.size() >= limit )
            break;
    }
    return matched;
}

}  // namespace mrst
